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
void net_dhcp_option(unsigned char option, unsigned char len, struct pbuf *p, unsigned short offset) { }
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
bool snmp_transport_is_open(void) { return sock >= 0; }

err_t snmp_sendto(void *handle, struct pbuf *p, const ip_addr_t *dst, u16_t port)
{
	static uint8_t buf[2048];
	u16_t n = pbuf_copy_partial(p, buf, sizeof(buf), 0);

	(void)handle;
	(void)dst;
	(void)port;
	sendto(sock, buf, n, 0, (struct sockaddr *)&peer, sizeof(peer));
	return ERR_OK;
}

u8_t snmp_get_local_ip_for_dst(void *handle, const ip_addr_t *dst, ip_addr_t *result)
{
	(void)handle;
	(void)dst;
	(void)result;
	return 0;
}

int main(void)
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
		snmp_receive(&sock, p, &src, ntohs(peer.sin_port));
		pbuf_free(p);
	}
}
