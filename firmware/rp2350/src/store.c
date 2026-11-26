/* Small persistent values, see store.h */

#include <stddef.h>
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
	if (store.update_pending == 0xffffffff)		/* saved before there was one */
		store.update_pending = 0;
	if (memcmp(store.unifi_magic, "UNF1", 4)) {
		memset(store.unifi_magic, 0, sizeof(store) - offsetof(struct store, unifi_magic));
		memcpy(store.unifi_magic, "UNF1", 4);
	}
	store.unifi_url[sizeof(store.unifi_url) - 1] = 0;
	store.unifi_bad[sizeof(store.unifi_bad) - 1] = 0;
}


bool store_save(void)
{
	return plat_sector_write(SECTOR_STORE, (const uint8_t *)&store, sizeof(store));
}
