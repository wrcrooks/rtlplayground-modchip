/*
 * SNMPv2c agent, read-only, on lwIP's SNMP core: its settings ("snmp ..."
 * in rtl_ext.c). The MIB is in snmp_mib.c, the UDP socket in snmp_udp.c.
 */

#include <stdio.h>
#include <string.h>

#include "lwip/apps/snmp.h"
#include "lwip/apps/snmp_mib2.h"

#include "bridge.h"
#include "net.h"
#include "plat.h"
#include "snmp_modchip.h"

static bool initialised;
static char community[33] = "public";
static char location[64];
static char contact[64];
static char descr[96];
static u16_t location_len, contact_len, descr_len, name_len;

static void agent_init(void)
{
	if (initialised)
		return;
	initialised = true;
	snprintf(descr, sizeof(descr), "RTLPlayground mod chip %s, %s", MODCHIP_VERSION, bridge_hello.machine);
	descr_len = strlen(descr);
	snmp_set_mibs((const struct snmp_mib **)snmp_modchip_mibs, 1);
	snmp_mib2_set_sysdescr((const u8_t *)descr, &descr_len);
	snmp_mib2_set_sysname((u8_t *)hostname, &name_len, 0);
	snmp_mib2_set_syslocation((u8_t *)location, &location_len, 0);
	snmp_mib2_set_syscontact((u8_t *)contact, &contact_len, 0);
	snmp_set_community(community);
	snmp_set_community_write("");		/* no writes */
	snmp_set_community_trap(community);
}


bool plat_snmp_set(int what, const char *v)
{
	char *dst;
	size_t max;

	switch (what) {
	case PLAT_SNMP_ENABLED:
		if (!v) {
			snmp_transport_close();
			return true;
		}
		agent_init();
		return snmp_transport_open();
	case PLAT_SNMP_COMMUNITY: dst = community; max = sizeof(community); break;
	case PLAT_SNMP_LOCATION: dst = location; max = sizeof(location); break;
	case PLAT_SNMP_CONTACT: dst = contact; max = sizeof(contact); break;
	default: return false;
	}
	if (!v)
		v = "";
	if (strlen(v) >= max)
		return false;
	strcpy(dst, v);
	location_len = strlen(location);
	contact_len = strlen(contact);
	return true;
}


const char *plat_snmp_get(int what)
{
	switch (what) {
	case PLAT_SNMP_ENABLED: return snmp_transport_is_open() ? "1" : NULL;
	case PLAT_SNMP_COMMUNITY: return community;
	case PLAT_SNMP_LOCATION: return location;
	case PLAT_SNMP_CONTACT: return contact;
	}
	return NULL;
}


/* Once a second: sysName follows the hostname, the port cache ages */
void snmp_second(void)
{
	name_len = strlen(hostname);
}
