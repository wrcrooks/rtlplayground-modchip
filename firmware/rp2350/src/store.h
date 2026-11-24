#ifndef _STORE_H_
#define _STORE_H_

/*
 * What has to outlive a restart besides the configuration: one flash sector
 * of small values, and one holding the configuration a rollback returns to.
 */

#include <stdbool.h>
#include <stdint.h>

struct store {
	char magic[8];
	uint32_t snmp_boots;		/* SNMPv3 snmpEngineBoots */
	uint8_t rollback;		/* enum rollback */
	uint8_t pad;
	uint16_t ssh_key_len;
	uint8_t ssh_key[160];		/* the SSH host key, DER */
	uint32_t update_seq;		/* firmware last accepted, tools/sign_update.py's sequence number */
	uint32_t update_pending;	/* the one being tried, 0: none */
	uint16_t rollback_min;		/* ROLLBACK_RESTORED: minutes to confirm in */
	uint16_t pad2;
};

/* PENDING: the snapshot goes back at the next start. RESTORED: the next start is
 * the one that takes up a restored configuration; the time to confirm it starts then */
enum rollback { ROLLBACK_NONE, ROLLBACK_PENDING, ROLLBACK_RESTORED };

extern struct store store;
void store_load(void);
bool store_save(void);

/* main.c: the sectors below the configuration's and the certificate's */
enum plat_sector { SECTOR_STORE, SECTOR_SNAPSHOT };
void plat_sector_read(int id, uint8_t *dst, uint32_t len);
bool plat_sector_write(int id, const uint8_t *src, uint32_t len);

#endif
