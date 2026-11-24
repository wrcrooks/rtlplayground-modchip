/* Firmware updates of the mod chip, see update.h */

#include <stdio.h>
#include <string.h>

#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"

#include "events.h"
#include "plat.h"
#include "store.h"
#include "update.h"
#include "upload.h"		/* the states */

#define PROVE_S		60		/* a new image runs healthy that long before it is kept */
#define GIVE_UP_S	300

static int state = UP_IDLE;
static char error[100];
static uint32_t body_left, image_len, got, checked;
static uint8_t hdr[UPDATE_HDR];
static uint8_t first[UPDATE_SECTOR], sector[UPDATE_SECTOR];
static mbedtls_sha256_context sha;
static bool sha_on;
static uint16_t healthy_s, running_s;

static bool fail(const char *why)
{
	snprintf(error, sizeof(error), "%s", why);
	state = UP_FAILED;
	return false;
}


static uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}


static bool signature_ok(void)
{
	mbedtls_ecp_group g;
	mbedtls_ecp_point q;
	mbedtls_mpi r, s;
	uint8_t h[32];
	bool ok;

	mbedtls_ecp_group_init(&g);
	mbedtls_ecp_point_init(&q);
	mbedtls_mpi_init(&r);
	mbedtls_mpi_init(&s);
	ok = !mbedtls_sha256(hdr, 128, h, 0)
		&& !mbedtls_ecp_group_load(&g, MBEDTLS_ECP_DP_SECP256R1)
		&& !mbedtls_ecp_point_read_binary(&g, &q, update_key, 65)
		&& !mbedtls_ecp_check_pubkey(&g, &q)
		&& !mbedtls_mpi_read_binary(&r, hdr + 128, 32)
		&& !mbedtls_mpi_read_binary(&s, hdr + 160, 32)
		&& !mbedtls_ecdsa_verify(&g, h, 32, &q, &r, &s);
	mbedtls_mpi_free(&s);
	mbedtls_mpi_free(&r);
	mbedtls_ecp_point_free(&q);
	mbedtls_ecp_group_free(&g);
	return ok;
}


/* Nothing is written before all of it holds */
static bool header_ok(void)
{
	struct update_slots s;

	if (memcmp(hdr, "MCUPD1\0", 8))
		return fail("not an update file");
	if (!signature_ok())
		return fail("the signature is not from the key this firmware trusts");
	if (memchr(hdr + 8, 0, 48) == NULL || memchr(hdr + 56, 0, 32) == NULL)
		return fail("malformed header");
	if (strcmp((char *)hdr + 8, update_board)) {
		snprintf(error, sizeof(error), "the update is for %.47s", (char *)hdr + 8);
		state = UP_FAILED;
		return false;
	}
	if (le32(hdr + 88) < store.update_seq)
		return fail("the update is older than one already installed");
	image_len = le32(hdr + 92);
	if (image_len != body_left)
		return fail("the file is not as long as it should be");
	update_slots(&s);
	if (image_len < UPDATE_SECTOR || image_len > s.other_size)
		return fail("the image doesn't fit the slot");
	return true;
}


bool update_begin(uint32_t content_length)
{
	struct update_slots s;

	update_abort();
	error[0] = 0;
	if (!update_has_key)
		return fail("this firmware was built without an update key");
	update_slots(&s);
	if (!s.ab)
		return fail("the flash has no A/B slots, see the README");
	if (s.trial)
		return fail("the running firmware is itself still on trial");
	if (content_length < UPDATE_HDR + UPDATE_SECTOR)
		return fail("too short for an update");
	body_left = content_length;
	got = checked = 0;
	state = UP_PREAMBLE;
	return true;
}


int update_state(void)
{
	return state;
}


const char *update_error(void)
{
	return error;
}


uint32_t update_body_left(void)
{
	return body_left;
}


/* A whole sector, or the last and shorter one */
static bool sector_done(uint32_t off, uint32_t n)
{
	memset(sector + n, 0xff, sizeof(sector) - n);
	if (off == 0) {
		/* The first one makes the slot bootable: it goes in last */
		memcpy(first, sector, sizeof(first));
		return update_flash_erase(0) || fail("erasing the flash failed");
	}
	return update_flash_write(off, sector) || fail("writing the flash failed");
}


size_t update_feed(const uint8_t *p, size_t n)
{
	size_t k;

	if (n > body_left)
		n = body_left;
	if (state == UP_PREAMBLE) {
		k = UPDATE_HDR - got;
		if (k > n)
			k = n;
		memcpy(hdr + got, p, k);
		got += k;
		body_left -= k;
		if (got == UPDATE_HDR && header_ok()) {
			state = UP_DATA;
			got = 0;
			mbedtls_sha256_init(&sha);
			mbedtls_sha256_starts(&sha, 0);
			sha_on = true;
		}
		return k;
	}
	if (state != UP_DATA) {		/* failed: the rest of the body goes nowhere */
		body_left -= n;
		return n;
	}
	uint32_t at = got % UPDATE_SECTOR;
	k = UPDATE_SECTOR - at;
	if (k > n)
		k = n;
	memcpy(sector + at, p, k);
	got += k;
	body_left -= k;
	if (got % UPDATE_SECTOR == 0 || got == image_len)
		sector_done((got - 1) / UPDATE_SECTOR * UPDATE_SECTOR, (got - 1) % UPDATE_SECTOR + 1);
	if (state == UP_DATA && got == image_len)
		state = UP_EPILOGUE;
	return k;
}


/* What is in the flash is what was signed: a sector per call */
bool update_finish(void)
{
	uint8_t h[32];

	if (state != UP_EPILOGUE) {
		if (state != UP_DONE && state != UP_FAILED)
			fail("the body ended early");
		return false;
	}
	if (checked < image_len) {
		uint32_t n = image_len - checked < UPDATE_SECTOR ? image_len - checked : UPDATE_SECTOR;

		if (checked)
			update_flash_read(checked, sector, n);
		mbedtls_sha256_update(&sha, checked ? sector : first, n);
		checked += n;
		return false;
	}
	mbedtls_sha256_finish(&sha, h);
	if (memcmp(h, hdr + 96, 32))
		return fail("the image is damaged");
	if (!update_flash_write(0, first))
		return fail("writing the flash failed");
	update_flash_read(0, sector, UPDATE_SECTOR);
	if (memcmp(sector, first, UPDATE_SECTOR)) {
		update_flash_erase(0);
		return fail("the flash reads back wrong");
	}
	state = UP_DONE;
	return true;
}


void update_abort(void)
{
	if (state == UP_DONE)
		update_flash_erase(0);
	if (sha_on)
		mbedtls_sha256_free(&sha);
	sha_on = false;
	body_left = 0;
	state = UP_IDLE;
}


bool update_ready(void)
{
	return state == UP_DONE;
}


void update_commit(void)
{
	store.update_pending = le32(hdr + 88);
	if (!store_save()) {
		event(EV_SYSTEM, 0, 0, "Update not started: the flash can't be written");
		store.update_pending = 0;
		update_abort();
		return;
	}
	update_flash_start();
}


void update_boot(void)
{
	struct update_slots s;

	healthy_s = running_s = 0;
	update_slots(&s);
	if (s.trial) {
		event(EV_SYSTEM, 0, 0, "Update %s started on trial", update_version);
	} else if (store.update_pending) {
		if (update_own_seq() != store.update_pending)
			event(EV_SYSTEM, 0, 0, "The update failed, back to %s", update_version);
		store.update_pending = 0;
		store_save();
	}
	if (!s.trial && update_own_seq() > store.update_seq) {
		/* flashed over SWD, or kept just as the power went */
		store.update_seq = update_own_seq();
		store_save();
	}
}


bool update_keep(void)
{
	struct update_slots s;

	update_slots(&s);
	if (!s.trial)
		return false;
	if (!update_flash_keep()) {
		event(EV_SYSTEM, 0, 0, "The update could not be kept");
		return false;
	}
	if (update_own_seq() > store.update_seq)
		store.update_seq = update_own_seq();
	store.update_pending = 0;
	store_save();
	event(EV_SYSTEM, 0, 0, "Update %s kept", update_version);
	return true;
}


/* healthy: the switch answers and the network is up */
void update_second(bool healthy)
{
	struct update_slots s;

	if (running_s > GIVE_UP_S)
		return;
	update_slots(&s);
	if (!s.trial) {
		running_s = GIVE_UP_S + 1;
		return;
	}
	running_s++;
	healthy_s = healthy ? healthy_s + 1 : 0;
	if (healthy_s >= PROVE_S && update_keep()) {
		running_s = GIVE_UP_S + 1;
	} else if (running_s > GIVE_UP_S) {
		event(EV_SYSTEM, 0, 0, "The update did not work, going back");
		plat_restart();		/* what wasn't kept doesn't start again */
	}
}


static const char *slot_name(const struct update_slots *s)
{
	return !s->ab ? "none" : s->running ? "B" : "A";
}


size_t update_status(char *buf, size_t max)
{
	struct update_slots s;
	size_t n;

	update_slots(&s);
	n = app(buf, max, 0, "Firmware %s for %s\n", update_version, update_board);
	if (!s.ab)
		n = app(buf, max, n, "No A/B slots in the flash: no updates over the network\n");
	else
		n = app(buf, max, n, "Slot %s%s, %lu KB for an update\n", slot_name(&s), s.trial ? ", on trial" : "",
			(unsigned long)s.other_size / 1024);
	if (!update_has_key)
		n = app(buf, max, n, "Built without an update key\n");
	return app(buf, max, n, "Updates from number %lu on\n", (unsigned long)store.update_seq);
}


size_t update_json(char *buf, size_t max)
{
	struct update_slots s;

	update_slots(&s);
	return app(buf, max, 0, "{\"version\":\"%s\",\"board\":\"%s\",\"slot\":\"%s\",\"trial\":%d,\"key\":%d,\"size\":%lu,\"seq\":%lu}",
		   update_version, update_board, slot_name(&s), s.trial, update_has_key, (unsigned long)s.other_size,
		   (unsigned long)store.update_seq);
}
