/*
 * update.c with a simulated slot, for run_update.py:
 *   test_update <file> [seq=<accepted>] [noslots] [trial] [break=<offset>] [stop=<bytes>]
 *	feeds the file as an upload; prints the verdict and whether the slot could boot
 *   test_update life		what happens to an update after it starts
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "events.h"
#include "plat.h"
#include "store.h"
#include "update.h"
#include "upload.h"
#include "update_key.h"

#define SLOT	0x10000

const char *const update_board = "modchip TEST", *const update_version = "v-test";
const uint8_t update_key[65] = { UPDATE_KEY };
const bool update_has_key = true;

static struct update_slots slots = { .ab = true, .other_size = SLOT };
static uint8_t flash[SLOT];
static int writes, restarts, kept, broken = -1;
static bool first_written_early;
static uint32_t own_seq = 100000;
static uint8_t store_flash[4096];

void update_slots(struct update_slots *s) { *s = slots; }
uint32_t update_own_seq(void) { return own_seq; }
bool update_flash_write(uint32_t off, const uint8_t *p)
{
	if (off % 4096 || off + 4096 > SLOT)
		abort();
	memcpy(flash + off, p, 4096);
	if (broken == (int)off)
		flash[off + 77] ^= 4;
	writes++;
	if (!off && update_state() != UP_EPILOGUE)
		first_written_early = true;
	return true;
}
bool update_flash_erase(uint32_t off) { memset(flash + off, 0xff, 4096); return true; }
void update_flash_read(uint32_t off, uint8_t *p, uint32_t n) { memcpy(p, flash + off, n); }
void update_flash_start(void) { }
bool update_flash_keep(void) { kept++; slots.trial = false; return true; }
void plat_restart(void) { restarts++; }
uint32_t plat_uptime_s(void) { return 0; }
void plat_sector_read(int id, uint8_t *dst, uint32_t len) { (void)id; memcpy(dst, store_flash, len); }
bool plat_sector_write(int id, const uint8_t *src, uint32_t len) { (void)id; memcpy(store_flash, src, len); return true; }
static char last[100];
static void on_event(const struct event *e) { strcpy(last, e->text); }

static int failures;
static void check(int ok, const char *what)
{
	printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
	failures += !ok;
}

static int life(void)
{
	event_listen(on_event);
	store_load();
	update_boot();
	check(store.update_seq == 100000 && !last[0], "the first start: the running image's number counts");

	store.update_pending = own_seq = 100005;
	slots.trial = true;
	update_boot();
	check(strstr(last, "on trial") != NULL, "a new image starts on trial");
	for (int i = 0; i < 59; i++)
		update_second(true);
	check(!kept, "not kept before a minute");
	update_second(false);
	for (int i = 0; i < 59; i++)
		update_second(true);
	check(!kept, "the minute starts over when the switch is gone");
	update_second(true);
	check(kept == 1 && store.update_seq == 100005 && !store.update_pending, "healthy for a minute: kept");
	for (int i = 0; i < 400; i++)
		update_second(false);
	check(!restarts, "and then it stays, whatever happens");

	store.update_pending = own_seq = 100010;
	slots.trial = true;
	update_boot();
	for (int i = 0; i < 300; i++)
		update_second(i % 40 < 30);
	check(!restarts, "never healthy for a minute: five minutes' patience");
	update_second(false);
	check(restarts == 1 && kept == 1, "then a restart, not kept");
	slots.trial = false;
	own_seq = 100005;
	update_boot();
	check(strstr(last, "failed") && store.update_seq == 100005 && !store.update_pending, "back: says so, the number unchanged");
	last[0] = 0;
	update_boot();
	check(!last[0], "once");

	store.update_pending = own_seq = 100020;
	update_boot();
	check(!last[0] && store.update_seq == 100020 && !store.update_pending, "kept just as the power went: no complaint");
	check(!update_keep(), "nothing on trial, nothing to keep");
	slots.trial = true;
	update_boot();
	check(update_keep() && !slots.trial, "kept by hand");
	memset(&((struct store *)store_flash)->update_pending, 0xff, 4);
	store_load();
	check(!store.update_pending, "settings saved before this");
	return failures != 0;
}

int main(int argc, char **argv)
{
	static uint8_t f[1 << 20], old[4096];
	size_t n, off = 0, stop = sizeof(f);

	if (argc > 1 && !strcmp(argv[1], "life"))
		return life();
	FILE *fp = fopen(argv[1], "rb");
	n = fread(f, 1, sizeof(f), fp);
	fclose(fp);
	store_load();
	memset(flash, 0x11, sizeof(flash));	/* an older image */
	memcpy(old, flash, sizeof(old));
	for (int i = 2; i < argc; i++) {
		if (!strncmp(argv[i], "seq=", 4))
			store.update_seq = strtoul(argv[i] + 4, NULL, 10);
		if (!strncmp(argv[i], "stop=", 5))
			stop = strtoul(argv[i] + 5, NULL, 10);
		if (!strncmp(argv[i], "break=", 6))
			broken = strtoul(argv[i] + 6, NULL, 10);
		if (!strcmp(argv[i], "noslots"))
			slots.ab = false;
		if (!strcmp(argv[i], "trial"))
			slots.trial = true;
	}
	if (!update_begin(n)) {
		printf("NO: %s\n", update_error());
		return 0;
	}
	srand(n);
	while (update_body_left() && off < stop) {
		size_t k = 1 + rand() % 3000, used;

		if (k > n - off)
			k = n - off;
		used = update_feed(f + off, k);
		if (!used || used > k)
			abort();
		off += used;
	}
	if (off < stop)
		while (!update_finish() && update_state() != UP_FAILED)
			;
	else
		update_abort();
	printf("%s%s\n", update_state() == UP_DONE ? "OK" : update_state() == UP_FAILED ? "NO: " : "stopped", update_error());
	printf("slot %s, %d writes\n", n - UPDATE_HDR <= SLOT && !memcmp(flash, f + UPDATE_HDR, n - UPDATE_HDR) ? "good" : !memcmp(flash, old, 4096) ? "untouched"
	       : flash[0] == 0xff && !memcmp(flash, flash + 1, 4095) ? "blank" : "odd", writes);
	if (first_written_early)
		printf("FIRST SECTOR WRITTEN EARLY\n");
	return 0;
}
