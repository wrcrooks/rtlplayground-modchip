#ifndef _SNMP_MODCHIP_H_
#define _SNMP_MODCHIP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct snmp_mib;

/* snmp_mib.c: MIB-2 with the switch ports as interfaces */
extern const struct snmp_mib *const snmp_modchip_mibs[1];

/* snmp_udp.c: UDP port 161 */
bool snmp_transport_open(void);
void snmp_transport_close(void);
bool snmp_transport_is_open(void);
/* A notification to UDP port 162 there; false without a route yet */
bool snmp_transport_trap(const uint8_t ip[4], const uint8_t *d, size_t n);

/*
 * snmp.c: what a message gets. SNMP_PASS: for lwIP's agent as it is; else
 * the length of the reply in out, 0 for none.
 */
#define SNMP_PASS	((size_t)-1)
size_t snmp_message(uint8_t *msg, size_t len, uint8_t *out, size_t max);
/* From snmp_sendto(): true if the agent was answering snmp.c itself */
struct pbuf;
bool snmp_captured(void *handle, struct pbuf *p);

#endif
