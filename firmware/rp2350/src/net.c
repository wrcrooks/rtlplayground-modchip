/*
 * lwIP on the switch's CPU port: a netif whose frames travel over the
 * bridge link, the IP settings shared with RTLPlayground's command line,
 * lwIP's DHCP client behind dhcp_start(), and the UDP sockets of syslog.c,
 * dns.c and ntp.c.
 */

#include <string.h>

#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/ethip6.h"
#include "lwip/mld6.h"
#include "lwip/nd6.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/dhcp.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"
#include "netif/ethernet.h"

#include "events.h"
#include "net.h"
#include "plat.h"
#include "unifi.h"

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
	nif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_MLD6;
	nif->output = etharp_output;
	nif->output_ip6 = ethip6_output;
	nif->linkoutput = linkoutput;
	return ERR_OK;
}


/* ---- IPv6 ---- */

#define IP6_STATIC	1	/* address slots: 0 link-local, 1 ours, the rest SLAAC */

static bool ip6_on, ip6_gw_set, ip6_addr_set_;
static ip6_addr_t ip6_static, ip6_gw;

const ip6_addr_t *net_ip6_gw(struct netif *nif, const ip6_addr_t *dest)
{
	(void)nif; (void)dest;
	return ip6_gw_set ? &ip6_gw : NULL;
}


/*
 * Make room for the static address. SLAAC takes the first free slot, so without
 * a static address that slot may hold one of its: keep it, in another one.
 */
static void ip6_slot_free(void)
{
	u8_t st = netif_ip6_addr_state(&netif, IP6_STATIC);

	if (!ip6_addr_isinvalid(st) && !netif_ip6_addr_isstatic(&netif, IP6_STATIC)) {
		u32_t valid = netif_ip6_addr_valid_life(&netif, IP6_STATIC);
		u32_t pref = netif_ip6_addr_pref_life(&netif, IP6_STATIC);

		if (!ip6_addr_set_)
			return;		/* nothing needs the slot */
		for (s8_t i = IP6_STATIC + 1; i < LWIP_IPV6_NUM_ADDRESSES; i++) {
			ip6_addr_t a;

			if (!ip6_addr_isinvalid(netif_ip6_addr_state(&netif, i)))
				continue;
			a = *netif_ip6_addr(&netif, IP6_STATIC);
			netif_ip6_addr_set_state(&netif, IP6_STATIC, IP6_ADDR_INVALID);
			netif_ip6_addr_set(&netif, i, &a);
			netif_ip6_addr_set_valid_life(&netif, i, valid);
			netif_ip6_addr_set_pref_life(&netif, i, pref);
			netif_ip6_addr_set_state(&netif, i, st);
			return;
		}
	}
	netif_ip6_addr_set_state(&netif, IP6_STATIC, IP6_ADDR_INVALID);
}


static void ip6_apply(void)
{
	rtl_ip6(ip6_on);
	if (!netif_is_up(&netif))
		return;
	netif.ip6_autoconfig_enabled = ip6_on;
	if (!ip6_on) {
		for (s8_t i = 0; i < LWIP_IPV6_NUM_ADDRESSES; i++)
			netif_ip6_addr_set_state(&netif, i, IP6_ADDR_INVALID);
		return;
	}
	if (ip6_addr_isinvalid(netif_ip6_addr_state(&netif, 0)))
		netif_create_ip6_linklocal_address(&netif, 1);
	ip6_slot_free();
	/* a static address has an infinite lifetime, which lwIP writes as 0 */
	if (ip6_addr_set_) {
		netif_ip6_addr_set(&netif, IP6_STATIC, &ip6_static);
		netif_ip6_addr_set_valid_life(&netif, IP6_STATIC, IP6_ADDR_LIFE_STATIC);
		netif_ip6_addr_set_state(&netif, IP6_STATIC, IP6_ADDR_TENTATIVE);
	}
}


bool plat_ip6_enable(bool on)
{
	ip6_on = on;
	ip6_apply();
	return true;
}


static bool ip6_parse(const char *s, ip6_addr_t *a)
{
	char b[48];
	size_t n = strcspn(s, "/ ");

	/* lwIP takes the prefix of a static address for 64 bits long */
	if (n >= sizeof(b) || (s[n] == '/' && (strncmp(s + n, "/64", 3) || (s[n + 3] && s[n + 3] != ' '))))
		return false;
	memcpy(b, s, n);
	b[n] = 0;
	if (!ip6addr_aton(b, a))
		return false;
	return !ip6_addr_ismulticast(a) && !ip6_addr_isany(a) && !ip6_addr_isloopback(a);
}


bool plat_ip6_addr(const char *s)
{
	ip6_addr_t a;

	if (s && (!ip6_parse(s, &a) || ip6_addr_islinklocal(&a)))
		return false;
	ip6_addr_set_ = s != NULL;
	if (s)
		ip6_static = a;
	ip6_addr_clear_zone(&ip6_static);
	ip6_apply();
	return true;
}


bool plat_ip6_gw(const char *s)
{
	ip6_addr_t a;

	if (s && !ip6_parse(s, &a))
		return false;
	ip6_gw_set = s != NULL;
	if (s) {
		ip6_gw = a;
		ip6_addr_assign_zone(&ip6_gw, IP6_UNICAST, &netif);
	}
	nd6_clear_destination_cache();
	return true;
}


size_t plat_ip6_status(char *buf, size_t max)
{
	size_t n = app(buf, max, 0, "IPv6 %s\n", ip6_on ? "on" : "off");

	for (s8_t i = 0; ip6_on && i < LWIP_IPV6_NUM_ADDRESSES; i++) {
		u8_t st = netif_ip6_addr_state(&netif, i);

		if (ip6_addr_isinvalid(st))
			continue;
		n = app(buf, max, n, "%s/64 %s%s\n", ip6addr_ntoa(netif_ip6_addr(&netif, i)),
			i == 0 ? "link-local" : i == IP6_STATIC && ip6_addr_set_ ? "static" : "autoconfigured",
			ip6_addr_istentative(st) ? ", tentative" : ip6_addr_isduplicated(st) ? ", duplicate" : "");
	}
	if (ip6_gw_set)
		n = app(buf, max, n, "Gateway %s\n", ip6addr_ntoa(&ip6_gw));
	return n;
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
	/* the interface may have come back as another: start over */
	for (s8_t i = 0; i < LWIP_IPV6_NUM_ADDRESSES; i++)
		netif_ip6_addr_set_state(&netif, i, IP6_ADDR_INVALID);
	ip6_apply();
	/* The config replay may have asked for DHCP before the netif was up */
	if (dhcp_on)
		dhcp_start(&netif);
	net_poll();
}


bool net_is_up(void)
{
	return netif_ready && netif_is_up(&netif) && !ip4_addr_isany_val(*netif_ip4_addr(&netif));
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
		nd6_cleanup_netif(&netif);
		netif_ip6_addr_set_state(&netif, 0, IP6_ADDR_INVALID);
		ip6_apply();
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


/* Options 6 and 43 from a DHCP reply, see LWIP_HOOK_DHCP_PARSE_OPTION in lwipopts.h */
void net_dhcp_option(unsigned char option, unsigned char len, struct pbuf *p, unsigned short offset)
{
	if (option == 6 && len >= 4) {
		pbuf_copy_partial(p, dhcp_dns, 4, offset);
	} else if (option == 43) {
		uint8_t v[64];
		uint8_t n = len < sizeof(v) ? len : sizeof(v);

		if (pbuf_copy_partial(p, v, n, offset) == n)
			unifi_dhcp_option43(v, n);
	}
}

/* Vendor class "ubnt" while UniFi is on: DHCP servers give option 43 to those
 * (see LWIP_HOOK_DHCP_APPEND_OPTIONS in lwipopts.h) */
void net_dhcp_append(struct dhcp_msg *msg, uint8_t type, uint16_t *len)
{
	if (!unifi_enabled() || (type != DHCP_DISCOVER && type != DHCP_REQUEST) || *len + 6 + 1 > DHCP_OPTIONS_LEN)
		return;
	msg->options[(*len)++] = 60;	/* vendor class identifier */
	msg->options[(*len)++] = 4;
	memcpy(msg->options + *len, "ubnt", 4);
	*len += 4;
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
