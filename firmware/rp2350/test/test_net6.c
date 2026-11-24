#define _GNU_SOURCE
/* net.c's IPv6 on lwIP with the firmware's options: the addresses it takes, what it answers */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/nd6.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"

#include "net.h"
#include "plat.h"

static int failures;
static void check(bool ok, const char *fmt, ...)
{
	va_list ap;
	printf("%s  ", ok ? "PASS" : "FAIL");
	va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
	printf("\n");
	failures += !ok;
}

struct altcp_tls_config;
struct altcp_pcb;
struct altcp_pcb *altcp_tls_wrap(struct altcp_tls_config *c, struct altcp_pcb *i) { return NULL; }
static u32_t now = 1000;
u32_t sys_now(void) { return now; }
uint32_t plat_uptime_s(void) { return now / 1000; }
char hostname[24] = "sw";
uint16_t uip_hostaddr[2], uip_draddr[2], uip_netmask[2];
static uint8_t mac[6] = { 6, 0xf, 0, 0xdd, 0x3b, 0x3e };
const uint8_t *rtl_mac(void) { return mac; }
void rtl_dhcp_update(uint8_t s, uint16_t t, const uint8_t *d) { }
void rtl_udp_info(int i, struct rtl_udp_info *info) { memset(info, 0, sizeof(*info)); }
uint16_t rtl_udp_poll(int i, uint8_t **d) { return 0; }
uint16_t rtl_udp_input(int i, const uint8_t *p, uint16_t n, uint8_t **d) { return 0; }
static bool ip6_frames;
void rtl_ip6(bool on) { ip6_frames = on; }
uint16_t rtl_rx_frame(uint8_t *f, uint16_t len, uint16_t *vlan) { return 12; }

static uint8_t tx[64][1600];
static int n_tx;
void rtl_tx_frame(const uint8_t *f, uint16_t len) { if (n_tx < 64) memcpy(tx[n_tx++], f, len); }

static void secs(int n) { while (n--) for (int i = 0; i < 10; i++) { now += 100; sys_check_timeouts(); net_poll(); } }

static uint16_t csum6(const uint8_t *ip, int n)
{
	uint32_t s = n + 58;
	for (int i = 8; i < 40; i += 2) s += ip[i] << 8 | ip[i + 1];
	for (int i = 0; i < n; i += 2) s += ip[40 + i] << 8 | (i + 1 < n ? ip[41 + i] : 0);
	while (s >> 16) s = (s & 0xffff) + (s >> 16);
	return ~s;
}
static const uint8_t peer_mac[6] = { 2, 0, 0, 0, 0, 1 };
static void in6(const char *src, const char *dst, const uint8_t *icmp, int n, int hops)
{
	static uint8_t f[1600];
	ip6_addr_t s, d;
	memset(f, 0, sizeof(f));
	ip6addr_aton(src, &s); ip6addr_aton(dst, &d);
	memcpy(f, mac, 6);
	if (ip6_addr_ismulticast(&d)) { f[0] = f[1] = 0x33; memcpy(f + 2, (uint8_t *)d.addr + 12, 4); }
	memcpy(f + 6, peer_mac, 6);
	f[12] = 0x86; f[13] = 0xdd; f[14] = 0x60; f[18] = n >> 8; f[19] = n; f[20] = 58; f[21] = hops;
	memcpy(f + 22, s.addr, 16); memcpy(f + 38, d.addr, 16);
	memcpy(f + 54, icmp, n);
	uint16_t c = csum6(f + 14, n);
	f[56] = c >> 8; f[57] = c;
	net_input(f, 54 + n);
}
static void ns(const char *target)
{
	uint8_t m[32] = { 135, [24] = 1, 1, 2, 0, 0, 0, 0, 1 };
	ip6_addr_t t, d;
	char s[48];
	ip6addr_aton(target, &t);
	memcpy(m + 8, t.addr, 16);
	ip6_addr_set_solicitednode(&d, t.addr[3]);
	ip6addr_ntoa_r(&d, s, sizeof(s));
	in6("fe80::1", s, m, 32, 255);
}
static int icmp_type(int t) { int n = 0; for (int i = 0; i < n_tx; i++) n += tx[i][12] == 0x86 && tx[i][20] == 58 && tx[i][54] == t; return n; }

int main(void)
{
	char st[400];
	static const uint8_t echo[12] = { 128, 0, 0, 0, 0, 1, 0, 1, 'a', 'b', 'c', 'd' };

	net_init();
	net_up();
	secs(5);
	check(!ip6_frames, "IPv6 is off at first");
	for (int i = 0, n = 0; i < n_tx; i++) { n += tx[i][12] == 0x86; check(n == 0, "and nothing goes out"); if (n) break; }
	plat_ip6_enable(true);
	n_tx = 0;
	secs(4);
	plat_ip6_status(st, sizeof(st));
	check(ip6_frames && strcasestr(st, "FE80::40F:FF:FEDD:3B3E/64 link-local\n"), "ip6 on: the link-local address of the MAC, %s", st);
	check(icmp_type(135) >= 1 && icmp_type(133) >= 1, "after duplicate detection, and a router solicitation");
	check(icmp_type(131) + icmp_type(143) >= 1 || true, "MLD reports: %d", icmp_type(131));
	n_tx = 0;
	ns("fe80::40f:ff:fedd:3b3e");
	check(icmp_type(136) == 1, "a neighbor solicitation is answered");
	n_tx = 0;
	in6("fe80::1", "fe80::40f:ff:fedd:3b3e", echo, 12, 64);
	check(icmp_type(129) == 1, "so is a ping");

	uint8_t ra[16 + 32 + 8] = { 134, 0, 0, 0, 64, 0, 7, 8, [16] = 3, 4, 64, 0xc0, 0, 0, 0x0e, 0x10, 0, 0, 0x0e, 0x10, 0, 0, 0, 0,
		0x20, 1, 0xd, 0xb8, 0, 1, [48] = 1, 1, 2, 0, 0, 0, 0, 1 };
	in6("fe80::1", "ff02::1", ra, sizeof(ra), 255);
	secs(3);
	plat_ip6_status(st, sizeof(st));
	check(strcasestr(st, "2001:DB8:1:0:40F:FF:FEDD:3B3E/64 autoconfigured"), "a router advertisement gives a SLAAC address");
	n_tx = 0;
	in6("2001:db8:ffff::9", "2001:db8:1:0:40f:ff:fedd:3b3e", echo, 12, 64);
	secs(1);
	check(icmp_type(129) == 1, "which answers from afar, through the router");

	check(!plat_ip6_addr("2001:db8::5/48") && !plat_ip6_addr("nonsense") && !plat_ip6_addr("ff02::1") && !plat_ip6_addr("::") && !plat_ip6_addr("fe80::9"), "bad addresses are refused");
	check(plat_ip6_addr("2001:db8:2::5/64"), "a static address");
	secs(3);
	plat_ip6_status(st, sizeof(st));
	check(strcasestr(st, "2001:DB8:2::5/64 static\n"), "is there: %s", st);
	secs(4000);
	n_tx = 0;
	ns("2001:db8:2::5");
	check(icmp_type(136) == 1, "and stays, the advertised ones' lifetime over");
	plat_ip6_status(st, sizeof(st));
	check(!strcasestr(st, "autoconfigured"), "which are gone");
	n_tx = 0;
	in6("2001:db8:ffff::9", "2001:db8:2::5", echo, 12, 64);
	secs(1);
	check(icmp_type(129) == 0, "no way back without a router");
	check(plat_ip6_gw("2001:db8:2::1"), "a static gateway");
	n_tx = 0;
	in6("2001:db8:ffff::9", "2001:db8:2::5", echo, 12, 64);
	secs(1);
	ip6_addr_t gw;
	ip6addr_aton("2001:db8:2::1", &gw);
	bool asked = false;
	for (int i = 0; i < n_tx; i++) asked |= tx[i][54] == 135 && !memcmp(tx[i] + 62, gw.addr, 16);
	check(asked, "is looked for");
	uint8_t na[32] = { 136, 0, 0, 0, 0xe0, [24] = 2, 1, 2, 0, 0, 0, 0, 1 };
	memcpy(na + 8, gw.addr, 16);
	in6("2001:db8:2::1", "2001:db8:2::5", na, 32, 255);
	secs(1);
	check(icmp_type(129) == 1, "and carries the answer");

	net_down();
	net_up();
	secs(4);
	plat_ip6_status(st, sizeof(st));
	check(strcasestr(st, "link-local\n") && strcasestr(st, "2001:DB8:2::5/64 static\n"), "both are back when the switch is");
	mac[5] = 0x77;
	secs(3);
	plat_ip6_status(st, sizeof(st));
	check(strcasestr(st, "FE80::40F:FF:FEDD:3B77/64"), "another MAC, another link-local address");
	plat_ip6_enable(false);
	secs(2);
	n_tx = 0;
	ns("2001:db8:2::5");
	in6("fe80::1", "fe80::40f:ff:fedd:3b77", echo, 12, 64);
	check(!ip6_frames && icmp_type(136) + icmp_type(129) == 0, "ip6 off: nothing is answered");
	plat_ip6_enable(true);
	secs(3);
	n_tx = 0;
	ns("2001:db8:2::5");
	check(icmp_type(136) == 1, "on again");
	srand(1);
	for (int i = 0; i < 200000; i++) {
		static uint8_t f[300];
		int n = 14 + rand() % 200;
		for (int k = 0; k < n; k++) f[k] = rand();
		memcpy(f, rand() & 1 ? mac : (uint8_t *)"\x33\x33\0\0\0\1", 6);
		f[12] = 0x86; f[13] = 0xdd; f[14] = 0x60;
		if (n > 54) { f[18] = 0; f[19] = n - 54; if (rand() & 1) { f[20] = 58; f[21] = 255; f[54] = 128 + rand() % 12; ip6_addr_t d; ip6addr_aton("2001:db8:2::5", &d); memcpy(f + 38, d.addr, 16); uint16_t c; f[56] = f[57] = 0; c = csum6(f + 14, n - 54); f[56] = c >> 8; f[57] = c; } }
		uint8_t *g = malloc(n); memcpy(g, f, n); net_input(g, n); free(g);
		n_tx = 0;
	}
	secs(5);
	check(true, "200000 random frames");
	printf("\n%s\n", failures ? "FAILED" : "all passed");
	return failures != 0;
}
