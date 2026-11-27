#ifndef _LLDP_NBR_H_
#define _LLDP_NBR_H_

/*
 * LLDP neighbours: what the device on each front-panel port says about itself
 * (IEEE 802.1AB), from the LLDP frames trapped to the CPU while "lldp on".
 * rtl_rx_frame() hands them here; "neighbors" lists them, unifi.c reports
 * them to the controller. Plain C, no SDK: both halves use it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LLDP_NBR_PORTS	10	/* front-panel ports 1..10 */

struct lldp_nbr {
	uint32_t expires;	/* uptime s, 0: none */
	char chassis[24];	/* a MAC as aa:bb:cc:dd:ee:ff, else the ID as text */
	bool chassis_mac;
	char port[32];		/* port ID */
	char port_desc[32];
	char name[32];		/* system name */
	uint8_t ip[4];		/* management address, 0.0.0.0: none */
};

/* One LLDPDU, its TLVs (after the EtherType), received on front-panel port front */
void lldp_nbr_rx(uint8_t front, const uint8_t *tlv, uint16_t len, uint32_t now);
/* The neighbour on that port, NULL: none, or its TTL is over */
const struct lldp_nbr *lldp_nbr_get(uint8_t front, uint32_t now);
/* All of them, a line each */
size_t lldp_nbr_show(char *out, size_t max, uint32_t now);
void lldp_nbr_clear(void);

#endif
