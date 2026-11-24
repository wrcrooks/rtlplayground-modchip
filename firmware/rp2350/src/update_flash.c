/* The two firmware slots of update.c: the RP2350 boot ROM's A/B partitions */

#include <string.h>

#include "boot/picobin.h"
#include "boot/picoboot_constants.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/flash.h"
#include "pico/stdlib.h"

#include "modchip_version.h"
#include "update.h"
#include "update_key.h"		/* generated: UPDATE_KEY, or nothing */

/* sign_update.py finds these in the binary */
static const char board_mark[] = "MCBOARD:" MODCHIP_UPDATE_BOARD;
static const char version_mark[] = "MCVERSION:" MODCHIP_VERSION;
const char *const update_board = board_mark + 8;
const char *const update_version = version_mark + 10;
#ifdef UPDATE_KEY
const uint8_t update_key[65] = { UPDATE_KEY };
const bool update_has_key = true;
#else
const uint8_t update_key[65];
const bool update_has_key = false;
#endif

static uint32_t other_off, other_size;
static int8_t running = -1;
static bool looked, trial;

/* ROM calls want it word aligned; explicit_buy 4 KiB of it */
static uint8_t __attribute__((aligned(4))) work[4096];

static bool partition(int i, uint32_t *off, uint32_t *size)
{
	uint32_t b[3];
	int n = rom_get_partition_table_info(b, 3, PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION | (uint32_t)i << 24);

	if (n != 3 || !(b[0] & PT_INFO_PARTITION_LOCATION_AND_FLAGS))
		return false;
	uint32_t f = (b[1] & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
	uint32_t l = (b[1] & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
	*off = f * FLASH_SECTOR_SIZE;
	*size = (l + 1 - f) * FLASH_SECTOR_SIZE;
	/* never over the settings at the end of the flash */
	return l >= f && *off + *size <= PICO_FLASH_SIZE_BYTES - 16 * FLASH_SECTOR_SIZE;
}


static void look(void)
{
	boot_info_t info;

	if (looked)
		return;
	looked = true;
	if (!rom_get_boot_info(&info) || info.partition < 0 || info.partition > 1)
		return;			/* started from the bare flash */
	/* after a watchdog restart it isn't loaded */
	if (rom_load_partition_table(work, sizeof(work), false) < 0)
		return;
	if (rom_get_b_partition(0) != 1 || !partition(!info.partition, &other_off, &other_size))
		return;
	running = info.partition;
	trial = info.tbyb_and_update_info & BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING;
}


void update_slots(struct update_slots *s)
{
	look();
	s->ab = running >= 0;
	s->running = running == 1;
	s->trial = trial;
	s->other_size = s->ab ? other_size : 0;
}


uint32_t update_own_seq(void)
{
	const uint32_t *w = (const uint32_t *)XIP_BASE, *end = w + 1024;

	while (w < end && *w != PICOBIN_BLOCK_MARKER_START)
		w++;
	for (w++; w < end; ) {
		uint8_t kind = *w, size = *w >> 8;

		if (kind == PICOBIN_BLOCK_ITEM_1BS_VERSION)
			return w[1];
		if (kind == PICOBIN_BLOCK_ITEM_2BS_LAST || !size || kind & 0x80)
			break;
		w += size;
	}
	return 0;
}


struct op { uint32_t off; const uint8_t *p; };

static void program(void *arg)
{
	const struct op *o = arg;

	flash_range_erase(o->off, FLASH_SECTOR_SIZE);
	if (o->p)
		flash_range_program(o->off, o->p, FLASH_SECTOR_SIZE);
}


static bool flash_op(uint32_t off, const uint8_t *p)
{
	struct op o = { other_off + off, p };

	look();
	if (running < 0 || off % FLASH_SECTOR_SIZE || off + FLASH_SECTOR_SIZE > other_size)
		return false;
	return flash_safe_execute(program, &o, 100) == PICO_OK;
}


bool update_flash_write(uint32_t off, const uint8_t *p)
{
	return flash_op(off, p);
}


bool update_flash_erase(uint32_t off)
{
	return flash_op(off, NULL);
}


/* By its place in the flash: the XIP window shows the running slot */
void update_flash_read(uint32_t off, uint8_t *p, uint32_t n)
{
	memcpy(p, (const uint8_t *)XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + other_off + off, n);
}


void update_flash_start(void)
{
	stdio_flush();
	rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, 100, XIP_BASE + other_off, 0);
	watchdog_reboot(0, 0, 0);
	for (;;)
		tight_loop_contents();
}


bool update_flash_keep(void)
{
	if (rom_explicit_buy(work, sizeof(work)) != 0)
		return false;
	trial = false;
	return true;
}
