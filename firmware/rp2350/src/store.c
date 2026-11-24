/* Small persistent values, see store.h */

#include <string.h>

#include "store.h"

#define STORE_MAGIC	"MCSTOR1"

struct store store;

void store_load(void)
{
	plat_sector_read(SECTOR_STORE, (uint8_t *)&store, sizeof(store));
	if (memcmp(store.magic, STORE_MAGIC, sizeof(store.magic)) || store.ssh_key_len > sizeof(store.ssh_key)) {
		memset(&store, 0, sizeof(store));
		memcpy(store.magic, STORE_MAGIC, sizeof(store.magic));
	}
}


bool store_save(void)
{
	return plat_sector_write(SECTOR_STORE, (const uint8_t *)&store, sizeof(store));
}
