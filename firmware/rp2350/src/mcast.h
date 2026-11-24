#ifndef _MCAST_H_
#define _MCAST_H_

/*
 * IGMP and MLD snooping per VLAN, with a querier. mcast.c is the protocol,
 * rtl_mcast.c the switch: membership reports, leaves and queries are trapped
 * to the CPU, which keeps who wants what and relays them, reports to the
 * router ports and queries to the whole VLAN. Ports are logical ports.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MC_PORTS	10
#define MC_GROUPS	128
#define MC_ROUTERS	16
#define MC_VLANS	16

struct mc_config {
	bool on;
	bool querier;
	bool restrict_unknown;		/* IPv4 multicast nobody asked for: to router ports only */
	uint16_t interval;		/* between general queries, s */
	uint16_t routers;		/* ports set by hand */
};

extern struct mc_config mc_cfg;

void mc_reset(void);			/* all off, all forgotten */
void mc_apply(void);			/* after a change of mc_cfg */
/* A trapped frame without CPU and VLAN tags; true if it was IGMP or MLD */
bool mc_rx(uint8_t port, uint16_t vlan, const uint8_t *f, uint16_t len);
void mc_port_down(uint8_t port);
void mc_second(void);
size_t mc_json(char *buf, size_t max);
unsigned mc_group_count(void);

/* ---- supplied by the switch ---- */
uint16_t mc_hw_ports(void);
uint16_t mc_hw_up(void);
uint16_t mc_hw_lag(uint8_t port);	/* port and what it is aggregated with */
bool mc_hw_vlan(uint16_t vlan, uint16_t *members, uint16_t *tagged);
unsigned mc_hw_vlans(uint16_t *list, unsigned max);	/* the ports' own */
void mc_hw_entry(uint16_t vlan, const uint8_t *mac, uint16_t ports);	/* 0: remove */
void mc_hw_mode(bool on, bool restrict_unknown, uint16_t routers);
void mc_hw_tx(uint16_t ports, uint16_t vlan, bool tagged, const uint8_t *f, uint16_t len);
const uint8_t *mc_hw_mac(void);
const uint8_t *mc_hw_ip(void);
uint8_t mc_hw_front(uint8_t port);

#endif
