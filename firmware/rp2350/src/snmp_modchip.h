#ifndef _SNMP_MODCHIP_H_
#define _SNMP_MODCHIP_H_

#include <stdbool.h>

struct snmp_mib;

/* snmp_mib.c: MIB-2 with the switch ports as interfaces */
extern const struct snmp_mib *const snmp_modchip_mibs[1];

/* snmp_udp.c: UDP port 161 */
bool snmp_transport_open(void);
void snmp_transport_close(void);
bool snmp_transport_is_open(void);

#endif
