/*
 * SNMP agent, read-only: v2c on lwIP's SNMP core, v3 in front of it (usm.c),
 * and the notifications. Its settings ("snmp ..."
 * in rtl_ext.c). The MIB is in snmp_mib.c, the UDP socket in snmp_udp.c.
 */

#include <stdio.h>
#include <string.h>

#include "lwip/apps/snmp.h"
#include "lwip/apps/snmp_mib2.h"

#include "lwip/pbuf.h"

#include "bridge.h"
#include "events.h"
#ifndef MODCHIP_VERSION		/* the host tests define it */
#include "modchip_version.h"
#endif
#include "net.h"
#include "plat.h"
#include "snmp_modchip.h"
#include "store.h"
#include "usm.h"

#define TRAP_TO	2

void snmp_receive(void *handle, struct pbuf *p, const ip_addr_t *source_ip, u16_t port);	/* snmp_msg.h */

static bool initialised;
static char community[33] = "public";
static char location[64];
static char contact[64];
static char descr[96];
static bool v2c = true;
static bool engine_up, cold_due;
static uint32_t engine_since;
static struct { bool on; uint8_t ip[4]; char user[USM_NAME_MAX + 1]; } trap_to[TRAP_TO];
static u16_t location_len, contact_len, descr_len, name_len;

/* ---- v3 (usm.c) ---- */

static uint8_t *cap_buf;
static size_t cap_max, cap_len;
static char cap_handle;

bool snmp_captured(void *handle, struct pbuf *p)
{
	if (handle != &cap_handle)
		return false;
	cap_len = p->tot_len <= cap_max ? pbuf_copy_partial(p, cap_buf, cap_max, 0) : 0;
	return true;
}


size_t usm_agent(const uint8_t *msg, size_t len, uint8_t *out, size_t max)
{
	struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);

	if (!p)
		return 0;
	pbuf_take(p, msg, len);
	cap_buf = out;
	cap_max = max;
	cap_len = 0;
	snmp_receive(&cap_handle, p, IP_ADDR_ANY, 0);
	pbuf_free(p);
	return cap_len;
}


const char *usm_community(void) { return community; }
uint32_t usm_time(void) { return plat_uptime_s() - engine_since; }
uint32_t usm_uptime_cs(void) { return plat_uptime_s() * 100; }

void usm_random(uint8_t *p, size_t n)
{
	while (n--)
		*p++ = plat_random32() >> 8;
}


/* Keys hang on the engine ID, and replay protection on its boot count */
static void engine_init(void)
{
	if (engine_up)
		return;
	engine_up = true;
	store.snmp_boots = store.snmp_boots < 0x7fffffff ? store.snmp_boots + 1 : 1;
	store_save();
	engine_since = plat_uptime_s();
	usm_engine(rtl_mac(), store.snmp_boots);
}


size_t snmp_message(uint8_t *msg, size_t len, uint8_t *out, size_t max)
{
	size_t at = 2;

	if (len < 8 || msg[0] != 0x30)
		return 0;
	if (msg[1] & 0x80)
		at += msg[1] & 0x7f;
	if (at + 3 > len || msg[at] != 2 || msg[at + 1] != 1)
		return 0;
	if (msg[at + 2] != 3)
		return v2c ? SNMP_PASS : 0;
	engine_init();
	return usm_receive(msg, len, out, max);
}


void plat_snmp_v2c(bool on)
{
	v2c = on;
}


bool plat_snmp_user(const char *name, bool sha256, const char *auth, const char *priv)
{
	if (!auth)
		return usm_user_del(name);
	engine_init();
	return usm_user_set(name, sha256 ? USM_SHA256 : USM_SHA1, auth, priv);
}


/* ---- notifications ---- */

static const uint32_t t_std[] = { 1, 3, 6, 1, 6, 3, 1, 1, 5, 0 };	/* snmpTraps */
/* Net-SNMP's playpen, meant for what has no number of its own */
static const uint32_t t_own[] = { 1, 3, 6, 1, 4, 1, 8072, 9999, 9999, 837, 0, 0 };

static bool trap_all(struct usm_trap *t)
{
	static uint8_t b[600];
	bool sent = false;

	for (int i = 0; i < TRAP_TO; i++) {
		size_t n;

		if (!trap_to[i].on)
			continue;
		t->community = community;
		t->user = trap_to[i].user[0] ? trap_to[i].user : NULL;
		n = usm_trap(t, b, sizeof(b));
		sent |= n && snmp_transport_trap(trap_to[i].ip, b, n);
	}
	return sent;
}


static void trap_std(struct usm_trap *t, uint32_t which)
{
	memcpy(t->oid, t_std, sizeof(t_std));
	t->oid[9] = which;
	t->oid_len = 10;
}


static void trap_event(const struct event *e)
{
	struct usm_trap t = { .text = e->text };

	if (!snmp_transport_is_open())
		return;
	switch (e->type) {
	case EV_LINK: {
		struct rtl_port p;

		trap_std(&t, e->value ? 4 : 3);		/* linkUp, linkDown */
		t.port = e->port;
		t.if_oper = e->value ? 1 : 2;
		t.if_admin = rtl_port_read(e->port, &p) && !p.enabled ? 2 : 1;
		t.text = NULL;
		break;
	}
	case EV_LOGIN_FAILED:
		trap_std(&t, 5);			/* authenticationFailure */
		break;
	default:
		memcpy(t.oid, t_own, sizeof(t_own));
		t.oid[11] = 1 + e->type;
		t.oid_len = 12;
	}
	trap_all(&t);
}


bool plat_snmp_trap(const uint8_t ip[4], const char *user, bool on)
{
	int slot = -1;

	for (int i = 0; i < TRAP_TO; i++)
		if (trap_to[i].on && !memcmp(trap_to[i].ip, ip, 4))
			slot = i;
	if (!on) {
		if (slot >= 0)
			trap_to[slot].on = false;
		return slot >= 0;
	}
	for (int i = 0; slot < 0 && i < TRAP_TO; i++)
		if (!trap_to[i].on)
			slot = i;
	if (slot < 0 || (user && strlen(user) > USM_NAME_MAX))
		return false;
	trap_to[slot].on = true;
	memcpy(trap_to[slot].ip, ip, 4);
	strcpy(trap_to[slot].user, user ? user : "");
	event_listen(trap_event);
	return true;
}


size_t plat_snmp_status(char *buf, size_t max)
{
	size_t n = app(buf, max, 0, "SNMPv2c %s\n", v2c ? "on" : "off");
	const char *u;
	enum usm_auth a;
	uint8_t len;
	const uint8_t *id;
	bool priv;

	for (int i = 0; (u = usm_user(i, &a, &priv)) != NULL; i++)
		n = app(buf, max, n, "SNMPv3 user %s: %s%s\n", u, a == USM_SHA256 ? "SHA-256" : "SHA-1",
			priv ? ", AES-128" : ", not encrypted");
	if (engine_up) {
		n = app(buf, max, n, "Engine ID 0x");
		id = usm_engine_id(&len);
		while (len--)
			n = app(buf, max, n, "%02x", *id++);
		n = app(buf, max, n, "\n");
	}
	for (int i = 0; i < TRAP_TO; i++)
		if (trap_to[i].on)
			n = app(buf, max, n, "Traps to %u.%u.%u.%u as %s\n", trap_to[i].ip[0], trap_to[i].ip[1],
				trap_to[i].ip[2], trap_to[i].ip[3], trap_to[i].user[0] ? trap_to[i].user : "v2c");
	return n;
}


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
		cold_due = !snmp_transport_is_open();
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
	if (cold_due && snmp_transport_is_open()) {
		struct usm_trap t = { 0 };

		trap_std(&t, 1);
		cold_due = !trap_all(&t);
	}
}
