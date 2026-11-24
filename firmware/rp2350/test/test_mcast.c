/* IGMP/MLD snooping and querier (mcast.c) against a model of the switch */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "events.h"
#include "mcast.h"

static int failures;
static void check(bool ok, const char *fmt, ...)
{
	va_list ap;

	printf("%s  ", ok ? "PASS" : "FAIL");
	va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
	printf("\n");
	failures += !ok;
}

static uint32_t now;
uint32_t plat_uptime_s(void) { return now; }

static struct { uint16_t vlan, ports; uint8_t mac[6]; } ent[512];
static struct { uint16_t ports, vlan; bool tagged; uint8_t f[1600]; uint16_t len; } tx[64];
static int n_tx;
static uint16_t up = 0x3f, lag_mask, unknown = 0xffff;
static bool mode_on, mode_restrict;
static uint8_t ip[4] = { 10, 0, 0, 5 };
static const uint8_t mac[6] = { 2, 0, 0, 0, 0, 0x99 };

uint16_t mc_hw_ports(void) { return 0x3f; }
uint16_t mc_hw_up(void) { return up; }
uint16_t mc_hw_lag(uint8_t p) { return (lag_mask >> p) & 1 ? lag_mask : 1 << p; }
bool mc_hw_vlan(uint16_t v, uint16_t *m, uint16_t *t)
{
	if (v == 1) { *m = 0x23f; *t = 0x200; return true; }
	if (v == 20) { *m = 0x20f; *t = 0x201; return true; }	/* ports 0 (tagged), 1-3 */
	return false;
}
unsigned mc_hw_vlans(uint16_t *l, unsigned max) { (void)max; l[0] = 1; return 1; }
void mc_hw_entry(uint16_t vlan, const uint8_t *m, uint16_t ports)
{
	int f = -1;
	for (int i = 0; i < 512; i++) {
		if (ent[i].vlan == vlan && !memcmp(ent[i].mac, m, 6)) { f = i; break; }
		if (!ent[i].vlan && f < 0) f = i;
	}
	ent[f].vlan = ports ? vlan : 0;
	ent[f].ports = ports;
	memcpy(ent[f].mac, m, 6);
}
void mc_hw_mode(bool on, bool r, uint16_t routers) { mode_on = on; mode_restrict = r; unknown = routers; }
void mc_hw_tx(uint16_t ports, uint16_t vlan, bool tagged, const uint8_t *f, uint16_t len)
{
	if (n_tx == 64) return;
	tx[n_tx].ports = ports; tx[n_tx].vlan = vlan; tx[n_tx].tagged = tagged;
	memcpy(tx[n_tx].f, f, len); tx[n_tx++].len = len;
}
const uint8_t *mc_hw_mac(void) { return mac; }
const uint8_t *mc_hw_ip(void) { return ip; }
uint8_t mc_hw_front(uint8_t p) { return p + 1; }

static int entry(uint16_t vlan, const char *m)	/* ports, -1: none */
{
	for (int i = 0; i < 512; i++)
		if (ent[i].vlan == vlan && !memcmp(ent[i].mac, m, 6))
			return ent[i].ports;
	return -1;
}
static int entries(void) { int n = 0; for (int i = 0; i < 512; i++) n += ent[i].vlan != 0; return n; }
static void secs(int n) { while (n--) { now++; mc_second(); } }

static uint16_t csum(const uint8_t *p, int n, uint32_t s)
{
	for (; n > 1; n -= 2, p += 2) s += p[0] << 8 | p[1];
	if (n) s += p[0] << 8;
	while (s >> 16) s = (s & 0xffff) + (s >> 16);
	return ~s;
}
static void igmp(uint8_t port, uint16_t vlan, const char *src, uint8_t *m, int n)
{
	uint8_t *f = malloc(14 + 24 + n);	/* exactly: ASan sees a read past it */
	unsigned a[4];

	sscanf(src, "%u.%u.%u.%u", a, a + 1, a + 2, a + 3);
	memset(f, 0, 38);
	memcpy(f, "\x01\x00\x5e\x00\x00\x16\x02\x00\x00\x00\x00\x01\x08\x00\x46\xc0", 16);
	f[16] = (24 + n) >> 8; f[17] = 24 + n; f[22] = 1; f[23] = 2;
	for (int i = 0; i < 4; i++) f[26 + i] = a[i];
	f[34] = 0x94; f[35] = 4;
	m[2] = m[3] = 0;
	uint16_t c = csum(m, n, 0); m[2] = c >> 8; m[3] = c;
	memcpy(f + 38, m, n);
	mc_rx(port, vlan, f, 38 + n);
	free(f);
}
static void v2(uint8_t port, uint16_t vlan, uint8_t type, int a, int b, int c, int d)
{
	uint8_t m[8] = { type, 0, 0, 0, a, b, c, d };
	igmp(port, vlan, "10.0.0.77", m, 8);
}
static void mld(uint8_t port, uint16_t vlan, uint8_t *m, int n, uint8_t srclast)
{
	uint8_t *f = calloc(1, 14 + 48 + n);
	memcpy(f, "\x33\x33\x00\x00\x00\x16\x02\x00\x00\x00\x00\x01\x86\xdd\x60", 15);
	f[18] = (8 + n) >> 8; f[19] = 8 + n; f[21] = 1;
	f[22] = 0xfe; f[23] = 0x80; f[37] = srclast;
	f[38] = 0xff; f[39] = 2; f[53] = 0x16;
	f[54] = 58; f[56] = 5; f[57] = 2; f[60] = 1;
	m[2] = m[3] = 0;
	uint32_t s = n + 58;
	for (int i = 22; i < 54; i += 2) s += f[i] << 8 | f[i + 1];
	uint16_t c = csum(m, n, s); m[2] = c >> 8; m[3] = c;
	memcpy(f + 62, m, n);
	mc_rx(port, vlan, f, 62 + n);
	free(f);
}

int main(void)
{
#define M1 "\x01\x00\x5e\x01\x02\x03"
	mc_reset();
	check(!mc_rx(0, 1, (const uint8_t *)M1 "\0\0\0\0\0\0\x08\0E", 60), "off: nothing is taken");
	mc_cfg.on = true;
	mc_apply();
	check(mode_on && !mode_restrict, "on: the switch traps, unknown multicast floods");

	v2(1, 1, 0x16, 239, 1, 2, 3);
	check(entry(1, M1) == 2, "a v2 report on port 2 makes an entry");
	v2(2, 1, 0x16, 239, 1, 2, 3);
	check(entry(1, M1) == 6, "port 3 joins it");
	v2(1, 20, 0x16, 239, 1, 2, 3);
	check(entry(1, M1) == 6 && entry(20, M1) == 2, "VLAN 20 has its own");
	v2(3, 1, 0x16, 239, 129, 2, 3);
	check(entry(1, M1) == 14, "239.129.2.3 shares the MAC: the union");
	v2(3, 1, 0x17, 239, 129, 2, 3);
	secs(4);
	check(entry(1, M1) == 6, "its leave takes only port 4 out");
	v2(1, 1, 0x16, 224, 0, 0, 251);
	check(entry(1, "\x01\x00\x5e\x00\x00\xfb") == -1, "a link-local group is left to flood");
	v2(1, 1, 0x16, 10, 1, 2, 3);
	check(mc_group_count() == 2, "a unicast address is no group");

	/* routers */
	n_tx = 0;
	uint8_t q[8] = { 0x11, 100 };
	igmp(5, 1, "10.0.0.1", q, 8);
	check(n_tx == 1 && tx[0].ports == 0x1f && !tx[0].tagged, "a query goes on to the VLAN's other ports");
	check(entry(1, M1) == (6 | 0x20) && entry(20, M1) == 2, "and its port becomes a router port in that VLAN");
	n_tx = 0;
	v2(0, 1, 0x16, 239, 9, 9, 9);
	check(n_tx == 1 && tx[0].ports == 0x20, "a report goes to the router port only");
	n_tx = 0;
	v2(5, 1, 0x16, 239, 9, 9, 9);
	check(n_tx == 0, "not back where it came from");
	igmp(4, 1, "0.0.0.0", q, 8);
	check(entry(1, M1) == (6 | 0x20), "a query from 0.0.0.0 makes no router port");
	n_tx = 0;
	igmp(1, 20, "10.0.20.1", q, 8);
	check(n_tx == 2 && tx[0].tagged && tx[0].ports == 1 && tx[0].vlan == 20 && tx[1].ports == 0xc, "VLAN 20: tagged to port 1, untagged to 3 and 4");

	/* aging */
	secs(100);
	v2(1, 1, 0x16, 239, 1, 2, 3);
	secs(156);
	check(entry(1, M1) >= 0 && !(entry(1, M1) & 0x20), "a router not heard for 255 s is forgotten (%x)", entry(1, M1));
	secs(105);
	check(mc_group_count() == 0 && entries() == 0, "members without reports for 260 s are too");

	/* v3 */
	uint8_t r3[8 + 12 + 8 + 8] = { 0x22, 0, 0, 0, 0, 0, 0, 3,
		4, 0, 0, 0, 239, 1, 2, 3,		/* to EXCLUDE {}: join */
		3, 0, 0, 1, 239, 4, 4, 4, 10, 0, 0, 9,	/* to INCLUDE {src}: join */
		6, 0, 0, 0, 239, 5, 5, 5 };		/* BLOCK: nothing */
	igmp(2, 1, "10.0.0.7", r3, sizeof(r3));
	check(entry(1, M1) == 4 && entry(1, "\x01\x00\x5e\x04\x04\x04") == 4 && mc_group_count() == 2, "IGMPv3 records");
	uint8_t l3[16] = { 0x22, 0, 0, 0, 0, 0, 0, 1, 3, 0, 0, 0, 239, 1, 2, 3 };
	igmp(2, 1, "10.0.0.7", l3, 16);
	secs(2);
	check(entry(1, M1) == 4, "a v3 leave: still there for the last-member time");
	v2(2, 1, 0x16, 239, 1, 2, 3);
	secs(5);
	check(entry(1, M1) == 4, "another member behind the port answered: it stays");
	igmp(2, 1, "10.0.0.7", l3, 16);
	secs(3);
	check(entry(1, M1) == -1, "nobody did: gone");
	uint8_t bad[16] = { 0x22, 0, 0, 0, 0, 0, 0xff, 0xff, 4, 0, 0xff, 0xff, 239, 1, 2, 3 };
	igmp(2, 1, "10.0.0.7", bad, 16);
	check(entry(1, M1) == -1, "a record longer than the packet is ignored");
	uint8_t sum[8 + 38] = { 0 };
	memcpy(sum, "\x01\x00\x5e\x00\x00\x16\x02\x00\x00\x00\x00\x01\x08\x00\x46\xc0\x00\x20", 18);
	sum[23] = 2; sum[38] = 0x16; sum[42] = 239; sum[43] = 1; sum[44] = 2; sum[45] = 3;
	check(mc_rx(2, 1, sum, 46) && entry(1, M1) == -1, "a wrong checksum is ignored");

	/* link, LAG */
	v2(1, 1, 0x16, 239, 1, 2, 3);
	up &= ~2; mc_port_down(1);
	check(entry(1, M1) == -1, "link down takes the port out");
	up = 0x3f;
	lag_mask = 0x18;
	v2(3, 1, 0x16, 239, 1, 2, 3);
	check(entry(1, M1) == 0x18, "a LAG member's report counts for the LAG");
	up &= ~8; mc_port_down(3);
	check(entry(1, M1) == 0x18, "one member down: the LAG stays in");
	up = 0x3f;
	n_tx = 0;
	igmp(0, 1, "10.0.0.1", q, 8);
	check(n_tx == 1 && (tx[0].ports & 0x18) == 8, "one copy goes to the LAG (%x)", tx[0].ports);
	lag_mask = 0;

	/* MLD */
	uint8_t d[24] = { 131, 0, 0, 0, 0, 0, 0, 0, 0xff, 0x0e, [23] = 0x42 };
	mld(4, 1, d, 24, 7);
	check(entry(1, "\x33\x33\x00\x00\x00\x42") == 0x11, "an MLD report (and the router port)");
	uint8_t dl[24] = { 131, 0, 0, 0, 0, 0, 0, 0, 0xff, 0x02, [19] = 1, 0xff, [23] = 0x42 };
	mld(4, 1, dl, 24, 7);
	check(entry(1, "\x33\x33\xff\x00\x00\x42") == -1, "solicited-node groups are left to flood");
	uint8_t d2[28] = { 143, 0, 0, 0, 0, 0, 0, 1, 4, 0, 0, 0, 0xff, 0x05, [27] = 0x43 };
	mld(3, 1, d2, 28, 7);
	check(entry(1, "\x33\x33\x00\x00\x00\x43") == 9, "an MLDv2 record");
	uint8_t dn[24] = { 132, 0, 0, 0, 0, 0, 0, 0, 0xff, 0x0e, [23] = 0x42 };
	mld(4, 1, dn, 24, 7);
	secs(3);
	check(entry(1, "\x33\x33\x00\x00\x00\x42") == -1, "MLD done");
	uint8_t ns[24] = { 135 };
	uint8_t nsf[80] = { 0x33, 0x33 };
	nsf[12] = 0x86; nsf[13] = 0xdd; nsf[14] = 0x60; nsf[19] = 24; nsf[20] = 58;
	memcpy(nsf + 54, ns, 24);
	check(!mc_rx(1, 1, nsf, 78), "neighbor discovery is not taken");

	/* querier */
	secs(400);
	n_tx = 0;
	mc_cfg.querier = true;
	mc_apply();
	secs(1);
	check(n_tx >= 2 && tx[0].ports == 0x3f && tx[0].vlan == 1, "the querier asks at once, IPv4 and IPv6");
	check(!csum(tx[0].f + 14, 24, 0) && !csum(tx[0].f + 38, 12, 0) && tx[0].f[38] == 0x11 && tx[0].f[34] == 0x94
	      && !memcmp(tx[0].f + 26, ip, 4) && !memcmp(tx[0].f, "\x01\x00\x5e\x00\x00\x01", 6), "an IGMPv3 general query, checksums right");
	uint32_t s = 28 + 58;
	uint8_t *f6 = NULL;
	for (int i = 0; i < n_tx; i++) if (tx[i].f[12] == 0x86) f6 = tx[i].f;
	for (int i = 22; f6 && i < 54; i += 2) s += f6[i] << 8 | f6[i + 1];
	check(f6 && f6[62] == 130 && !csum(f6 + 62, 28, s) && f6[21] == 1, "an MLDv2 one too");
	n_tx = 0;
	secs(31);
	check(n_tx >= 2, "again after a quarter interval at the start");
	secs(40);
	n_tx = 0;
	secs(124);
	int a = n_tx;
	secs(125);
	check(a == 6 && n_tx - a == 6, "then every 125 s in both VLANs (%d, %d)", a, n_tx - a);
	v2(2, 1, 0x16, 239, 1, 2, 3);
	n_tx = 0;
	v2(2, 1, 0x17, 239, 1, 2, 3);
	check(n_tx == 1 && !memcmp(tx[0].f + 42, "\xef\x01\x02\x03", 4) && tx[0].f[3] == 1, "a leave is answered with a group query");
	igmp(5, 1, "10.0.0.9", q, 8);
	n_tx = 0;
	secs(130);
	bool own = false;
	for (int i = 0; i < n_tx; i++) own |= tx[i].vlan == 1 && tx[i].f[12] == 0x08;
	check(own, "a querier with a higher address doesn't stop us");
	igmp(5, 1, "10.0.0.1", q, 8);
	n_tx = 0;
	secs(250);
	own = false;
	for (int i = 0; i < n_tx; i++) own |= tx[i].vlan == 1 && tx[i].f[12] == 0x08;
	check(!own && n_tx > 0, "one with a lower address does, for IPv4 only");
	n_tx = 0;
	secs(10);
	own = false;
	for (int i = 0; i < n_tx; i++) own |= tx[i].vlan == 1 && tx[i].f[12] == 0x08;
	check(own, "gone for 255 s: we take over");
	memset(ip, 0, 4);
	n_tx = 0;
	secs(200);
	own = false;
	for (int i = 0; i < n_tx; i++) own |= tx[i].vlan == 1 && tx[i].f[12] == 0x08;
	check(!own, "no IPv4 query without an address");
	memcpy(ip, "\x0a\0\0\x05", 4);

	/* unknown */
	mc_cfg.restrict_unknown = true;
	mc_cfg.routers = 0x10;
	mc_apply();
	check(mode_restrict && unknown & 0x10 && entry(1, "\x01\x00\x5e\x00\x00\xfb") == 0x3f, "unknown to router ports; mDNS has an entry");
	v2(1, 1, 0x16, 239, 1, 2, 3);
	check(entry(1, M1) == 0x12, "static router ports are in");
	static char j[8192];
	mc_json(j, sizeof(j));
	check(strstr(j, "{\"vlan\":1,\"group\":\"239.1.2.3\",\"ports\":[2]}") != NULL, "JSON: %.60s", j);
	for (size_t m = 1; m < 600; m += 7) {
		char *t = malloc(m);
		if (mc_json(t, m) >= m) check(false, "JSON in %zu bytes", m);
		free(t);
	}

	/* full, off, and noise */
	for (int i = 0; i < 300; i++)
		v2(1, 1, 0x16, 239, 7, i >> 8, i);
	check(mc_group_count() == MC_GROUPS, "the table fills up");
	mc_cfg.on = false;
	mc_apply();
	check(!mode_on && entries() == 0, "off removes every entry");
	mc_cfg.on = true;
	mc_apply();
	srandom(1);
	for (int i = 0; i < 300000; i++) {
		int n = random() % 130;
		uint8_t *f = malloc(n ? n : 1);
		for (int k = 0; k < n; k++) f[k] = random();
		if (n > 60) {
			f[0] |= 1; f[12] = 8; f[13] = 0; f[14] = 0x45 + random() % 3; f[16] = 0; f[17] = random() % (n - 10); f[20] = f[21] = 0; f[23] = 2;
			if (random() & 1) { f[12] = 0x86; f[13] = 0xdd; f[14] = 0x60; f[18] = 0; f[19] = random() % (n - 40); f[20] = 0; f[54] = 58; f[55] = random() % 3; f[21] = 1; }
		}
		mc_rx(random() % 12, random() % 30, f, n);
		n_tx = 0;
		free(f);
	}
	secs(10);
	check(true, "300000 random frames");
	printf("\n%s\n", failures ? "FAILED" : "all passed");
	return failures != 0;
}
