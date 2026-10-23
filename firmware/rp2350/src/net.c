/*
 * lwIP on the switch's CPU port: a netif whose frames travel over the
 * bridge link, the IP settings shared with RTLPlayground's command line,
 * lwIP's DHCP client behind dhcp_start(), and the UDP sockets of syslog.c,
 * dns.c and ntp.c.
 */

#include <string.h>

#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/dhcp.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"
#include "netif/ethernet.h"

#include "net.h"
#include "plat.h"

/* DHCP_* of RTLPlayground's dhcp.h, which the command line prints */
#define RTLP_DHCP_OFF		0
#define RTLP_DHCP_DISCOVER	2
#define RTLP_DHCP_LEASING	4

static struct netif netif;
static bool netif_ready;
static bool dhcp_on;
static uint8_t frame_buf[1600];
static uint8_t dhcp_dns[4];
static uint32_t applied[3];	/* host, gateway, mask as last given to lwIP */

static struct udp_pcb *sock_pcb[RTL_UDP_CONNS];
static uint16_t sock_lport[RTL_UDP_CONNS];


static err_t linkoutput(struct netif *nif, struct pbuf *p)
{
	(void)nif;
	uint16_t len = pbuf_copy_partial(p, frame_buf, sizeof(frame_buf), 0);

	rtl_tx_frame(frame_buf, len);
	return ERR_OK;
}


static err_t netif_setup(struct netif *nif)
{
	nif->name[0] = 's';
	nif->name[1] = 'w';
	nif->mtu = 1500;
	nif->hwaddr_len = ETH_HWADDR_LEN;
	nif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
	nif->output = etharp_output;
	nif->linkoutput = linkoutput;
	return ERR_OK;
}


void net_init(void)
{
	lwip_init();
	netif_add(&netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL, netif_setup, ethernet_input);
	netif_set_default(&netif);
	netif_set_hostname(&netif, hostname);
	netif_ready = true;
}


void net_input(uint8_t *frame, uint16_t len)
{
	uint16_t vlan, off = rtl_rx_frame(frame, len, &vlan);

	if (!off || !netif_is_up(&netif))
		return;
	frame += off - 12;
	len -= off - 12;
	struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
	if (!p)
		return;
	pbuf_take(p, frame, len);
	if (netif.input(p, &netif) != ERR_OK)
		pbuf_free(p);
}


void net_up(void)
{
	memcpy(netif.hwaddr, rtl_mac(), ETH_HWADDR_LEN);
	memset(applied, 0, sizeof(applied));
	netif_set_up(&netif);
	netif_set_link_up(&netif);
	/* The config replay may have asked for DHCP before the netif was up */
	if (dhcp_on)
		dhcp_start(&netif);
	net_poll();
}


void net_down(void)
{
	if (dhcp_on)
		dhcp_release_and_stop(&netif);
	dhcp_on = false;
	netif_set_link_down(&netif);
	netif_set_down(&netif);
}


/*
 * The command line writes uip_hostaddr and friends directly ("ip", "gw",
 * "netmask", "mac"); follow them. Under DHCP it goes the other way.
 */
void net_poll(void)
{
	uint32_t want[3];

	if (!netif_ready || !netif_is_up(&netif))
		return;
	if (memcmp(netif.hwaddr, rtl_mac(), ETH_HWADDR_LEN)) {
		memcpy(netif.hwaddr, rtl_mac(), ETH_HWADDR_LEN);
		etharp_cleanup_netif(&netif);
	}
	if (dhcp_on) {
		if (dhcp_supplied_address(&netif)) {
			memcpy(uip_hostaddr, netif_ip4_addr(&netif), 4);
			memcpy(uip_draddr, netif_ip4_gw(&netif), 4);
			memcpy(uip_netmask, netif_ip4_netmask(&netif), 4);
		}
		return;
	}
	memcpy(&want[0], uip_hostaddr, 4);
	memcpy(&want[1], uip_draddr, 4);
	memcpy(&want[2], uip_netmask, 4);
	if (!memcmp(want, applied, sizeof(want)))
		return;
	memcpy(applied, want, sizeof(want));
	ip4_addr_t a, g, m;
	ip4_addr_set_u32(&a, want[0]);
	ip4_addr_set_u32(&g, want[1]);
	ip4_addr_set_u32(&m, want[2]);
	netif_set_addr(&netif, &a, &m, &g);
}


void plat_dhcp_start(void)
{
	netif_set_hostname(&netif, hostname);
	memset(dhcp_dns, 0, sizeof(dhcp_dns));
	dhcp_on = true;
	if (netif_is_up(&netif))
		dhcp_start(&netif);
	rtl_dhcp_update(RTLP_DHCP_DISCOVER, 0, NULL);
}


void plat_dhcp_stop(void)
{
	if (dhcp_on)
		dhcp_release_and_stop(&netif);
	dhcp_on = false;
	memset(applied, 0, sizeof(applied));	/* back to the static settings */
	rtl_dhcp_update(RTLP_DHCP_OFF, 0, NULL);
}


/* Option 6 from a DHCP reply, see LWIP_HOOK_DHCP_PARSE_OPTION in lwipopts.h */
void net_dhcp_option(unsigned char option, unsigned char len, struct pbuf *p, unsigned short offset)
{
	if (option == 6 && len >= 4)
		pbuf_copy_partial(p, dhcp_dns, 4, offset);
}


static void dhcp_report(void)
{
	struct dhcp *d = netif_dhcp_data(&netif);

	if (!dhcp_on)
		return;
	if (!d || !dhcp_supplied_address(&netif)) {
		rtl_dhcp_update(RTLP_DHCP_DISCOVER, 0, NULL);
		return;
	}
	uint32_t left = d->t0_timeout > d->lease_used ? (uint32_t)(d->t0_timeout - d->lease_used) * DHCP_COARSE_TIMER_SECS : 0;
	rtl_dhcp_update(RTLP_DHCP_LEASING, left > 0xffff ? 0xffff : left, dhcp_dns);
}

/* ---- UDP for syslog.c, dns.c and ntp.c ---- */

static void udp_send_to(int i, const uint8_t *data, uint16_t n)
{
	struct rtl_udp_info info;
	ip_addr_t dst;

	if (!n || !sock_pcb[i])
		return;
	rtl_udp_info(i, &info);
	IP_ADDR4(&dst, info.ripaddr[0], info.ripaddr[1], info.ripaddr[2], info.ripaddr[3]);
	struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, n, PBUF_RAM);
	if (!p)
		return;
	pbuf_take(p, data, n);
	udp_sendto(sock_pcb[i], p, &dst, info.rport);
	pbuf_free(p);
}


static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
	static uint8_t buf[RTL_UDP_BUF];
	int i = (int)(intptr_t)arg;
	uint8_t *out;
	(void)pcb;
	(void)addr;
	(void)port;

	uint16_t n = pbuf_copy_partial(p, buf, sizeof(buf), 0);
	pbuf_free(p);
	n = rtl_udp_input(i, buf, n, &out);
	udp_send_to(i, out, n);
}


/* Each tick: follow the connections the modules opened and closed, and poll them */
void net_udp_poll(void)
{
	if (!netif_ready)
		return;
	for (int i = 0; i < RTL_UDP_CONNS; i++) {
		struct rtl_udp_info info;
		uint8_t *out;

		rtl_udp_info(i, &info);
		if (info.lport != sock_lport[i] && sock_pcb[i]) {
			udp_remove(sock_pcb[i]);
			sock_pcb[i] = NULL;
		}
		sock_lport[i] = info.lport;
		if (!info.lport)
			continue;
		if (!sock_pcb[i]) {
			sock_pcb[i] = udp_new();
			if (!sock_pcb[i])
				continue;
			udp_bind(sock_pcb[i], IP4_ADDR_ANY, info.lport);
			udp_recv(sock_pcb[i], udp_recv_cb, (void *)(intptr_t)i);
		}
		if (netif_is_up(&netif) && !ip4_addr_isany_val(*netif_ip4_addr(&netif))) {
			uint16_t n = rtl_udp_poll(i, &out);
			udp_send_to(i, out, n);
		}
	}
}


void net_second(void)
{
	dhcp_report();
}
