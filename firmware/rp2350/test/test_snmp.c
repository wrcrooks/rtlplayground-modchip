/*
 * The SNMP agent on the host: lwIP built for the PC with the firmware's
 * options, the firmware's MIB and settings (snmp_mib.c, snmp.c), and an OS
 * UDP socket on 127.0.0.1 in place of snmp_udp.c. Six simulated ports.
 * run_snmp.py starts it and queries it with net-snmp's snmpget/snmpwalk.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

#include "lwip/apps/snmp.h"
#include "lwip/init.h"
#include "lwip/pbuf.h"

#include "bridge.h"
#include "plat.h"
#include "snmp_modchip.h"
#include "events.h"
#include "store.h"
#include <stdlib.h>

void snmp_receive(void *handle, struct pbuf *p, const ip_addr_t *source_ip, u16_t port);
extern void *snmp_traps_handle;

struct bridge_hello bridge_hello = { .machine = "SWGT024 V2.0 Unmanaged" };
char hostname[24] = "sim-switch";
static const uint8_t mac[6] = { 0x06, 0x0f, 0x00, 0xdd, 0x3b, 0x3e };

u32_t sys_now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

bool bridge_is_up(void) { return true; }
const struct ip6_addr *net_ip6_gw(struct netif *n, const struct ip6_addr *d) { return NULL; }
void net_dhcp_option(unsigned char option, unsigned char len, struct pbuf *p, unsigned short offset) { }
void net_dhcp_append(struct dhcp_msg *msg, unsigned char type, unsigned short *len) { }
const uint8_t *rtl_mac(void) { return mac; }
uint8_t rtl_port_count(void) { return 6; }

/* Port 3 is up at 1G with counters past 2^32, port 5 is disabled */
bool rtl_port_read(uint8_t phys, struct rtl_port *p)
{
	if (phys < 1 || phys > 6)
		return false;
	memset(p, 0, sizeof(*p));
	snprintf(p->name, sizeof(p->name), phys == 3 ? "uplink" : "Port %u", phys);
	p->enabled = phys != 5;
	p->sfp = phys == 6;
	p->link = phys == 3 ? 3 : 0;
	p->in_octets = phys == 3 ? 0x100000123ull : 1000 * phys;
	p->out_octets = 2000 * phys;
	p->in_ucast = 10 * phys;
	p->in_mcast = 3;
	p->in_bcast = 4;
	p->out_ucast = 20 * phys;
	p->in_errors = phys == 3 ? 7 : 0;
	return true;
}

uint8_t rtl_ports_read(struct rtl_port *p, uint8_t max)
{
	uint8_t n = 0;

	while (n < max && rtl_port_read(n + 1, &p[n]))
		n++;
	return n;
}

/* ---- the transport: an OS socket ---- */

static int sock = -1;
static struct sockaddr_in peer;

bool snmp_transport_open(void)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	socklen_t len = sizeof(a);

	if (sock >= 0)
		return true;
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0 || bind(sock, (struct sockaddr *)&a, sizeof(a)))
		return false;
	getsockname(sock, (struct sockaddr *)&a, &len);
	printf("PORT %u\n", ntohs(a.sin_port));
	fflush(stdout);
	snmp_traps_handle = &sock;
	return true;
}

void snmp_transport_close(void) { }
static bool sock_off;
bool snmp_transport_is_open(void) { return sock >= 0 && !sock_off; }

err_t snmp_sendto(void *handle, struct pbuf *p, const ip_addr_t *dst, u16_t port)
{
	static uint8_t buf[2048];
	u16_t n = pbuf_copy_partial(p, buf, sizeof(buf), 0);

	(void)handle;
	(void)dst;
	(void)port;
	if (snmp_captured(handle, p))
		return ERR_OK;
	sendto(sock, buf, n, 0, (struct sockaddr *)&peer, sizeof(peer));
	return ERR_OK;
}

uint32_t plat_uptime_s(void) { return sys_now() / 1000; }
uint32_t plat_random32(void) { return random(); }
void plat_sector_read(int id, uint8_t *dst, uint32_t len) { memset(dst, 0xff, len); }
bool plat_sector_write(int id, const uint8_t *src, uint32_t len) { return true; }

static int trap_port;
bool snmp_transport_trap(const uint8_t ip[4], const uint8_t *d, size_t n)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons(trap_port) };

	return trap_port && sendto(sock, d, n, 0, (struct sockaddr *)&a, sizeof(a)) == (ssize_t)n;
}

u8_t snmp_get_local_ip_for_dst(void *handle, const ip_addr_t *dst, ip_addr_t *result)
{
	(void)handle;
	(void)dst;
	(void)result;
	return 0;
}

static void cold(void)
{
	plat_snmp_set(PLAT_SNMP_ENABLED, NULL);
	sock_off = true;
	plat_snmp_set(PLAT_SNMP_ENABLED, "1");
	sock_off = false;
	snmp_second();
}

int main(int argc, char **argv)
{
	static uint8_t buf[2048];
	struct timeval tv = { .tv_sec = 30 };

	lwip_init();
	plat_snmp_set(PLAT_SNMP_COMMUNITY, "sim");
	plat_snmp_set(PLAT_SNMP_LOCATION, "test bench");
	plat_snmp_set(PLAT_SNMP_CONTACT, "nobody");
	if (!plat_snmp_set(PLAT_SNMP_ENABLED, "1"))
		return 1;
	snmp_second();
	store_load();
	store.snmp_boots = 6;
	plat_snmp_user("v3sha", false, "authpassword", "privpassword");
	plat_snmp_user("v3sha256", true, "authpassword", "privpassword");
	plat_snmp_user("v3auth", true, "authpassword", NULL);
	if (plat_snmp_user("short", true, "short", NULL))
		return 1;
	if (argc > 1) {
		trap_port = atoi(argv[1]);
		plat_snmp_trap((uint8_t *)"\x7f\0\0\x01", NULL, true);
		plat_snmp_trap((uint8_t *)"\x7f\0\0\x02", "v3sha256", true);
		plat_snmp_set(PLAT_SNMP_ENABLED, "1");
	}
	{ static char st[600]; plat_snmp_status(st, sizeof(st)); for (char *p = st; *p; p++) if (*p == '\n') *p = '|'; printf("STATUS %s\n", st); fflush(stdout); }
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	for (;;) {
		socklen_t len = sizeof(peer);
		ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &len);
		if (n <= 0)
			return 0;	/* idle: the runner is done */
		struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, n, PBUF_RAM);
		ip_addr_t src;
		pbuf_take(p, buf, n);
		IP_ADDR4(&src, 127, 0, 0, 1);
		static uint8_t out[2048];
		size_t k = snmp_message(buf, n, out, sizeof(out));
		if (k != SNMP_PASS && k)
			sendto(sock, out, k, 0, (struct sockaddr *)&peer, sizeof(peer));
		/* a datagram saying "EVENT" makes some */
		if (n == 5 && !memcmp(buf, "EVENT", 5)) {
			cold();
			event(EV_LINK, 3, 0, "Port 3 link down");
			event(EV_LOGIN_FAILED, 0, 0, "Web login failed from 10.0.0.9");
			event(EV_ERRDISABLE, 4, 1, "Port 4 error-disabled: link-flap");
		}
		if (n == 6 && !memcmp(buf, "V2COFF", 6))
			plat_snmp_v2c(false);
		if (k == SNMP_PASS)
			snmp_receive(&sock, p, &src, ntohs(peer.sin_port));
		pbuf_free(p);
	}
}
