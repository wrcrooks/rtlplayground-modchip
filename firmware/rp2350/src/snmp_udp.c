/*
 * The agent's UDP socket on lwIP, in place of lwIP's snmp_raw.c, so that
 * "snmp off" can close port 161 again.
 */

#include "lwip/apps/snmp.h"
#include "lwip/ip.h"
#include "lwip/udp.h"

#include "snmp_modchip.h"
#include "usm.h"

/* lwIP's SNMP core, from snmp_msg.h and snmp_traps.c */
void snmp_receive(void *handle, struct pbuf *p, const ip_addr_t *source_ip, u16_t port);
extern void *snmp_traps_handle;

static struct udp_pcb *pcb;

static void snmp_recv(void *arg, struct udp_pcb *upcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
	static uint8_t in[USM_MSG_MAX], out[USM_MSG_MAX];
	u16_t len = pbuf_copy_partial(p, in, sizeof(in), 0);
	size_t n = p->tot_len > sizeof(in) ? 0 : snmp_message(in, len, out, sizeof(out));

	(void)arg;
	if (n == SNMP_PASS) {
		snmp_receive(upcb, p, addr, port);
	} else if (n) {
		struct pbuf *r = pbuf_alloc(PBUF_TRANSPORT, n, PBUF_RAM);

		if (r) {
			pbuf_take(r, out, n);
			udp_sendto(upcb, r, addr, port);
			pbuf_free(r);
		}
	}
	pbuf_free(p);
}


err_t snmp_sendto(void *handle, struct pbuf *p, const ip_addr_t *dst, u16_t port)
{
	if (snmp_captured(handle, p))
		return ERR_OK;
	return udp_sendto((struct udp_pcb *)handle, p, dst, port);
}


u8_t snmp_get_local_ip_for_dst(void *handle, const ip_addr_t *dst, ip_addr_t *result)
{
	struct udp_pcb *upcb = handle;
	struct netif *dst_if;
	const ip_addr_t *dst_ip;

	(void)upcb;
	ip_route_get_local_ip(&upcb->local_ip, dst, dst_if, dst_ip);
	if (dst_if && dst_ip) {
		ip_addr_copy(*result, *dst_ip);
		return 1;
	}
	return 0;
}


bool snmp_transport_open(void)
{
	if (pcb)
		return true;
	pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
	if (!pcb)
		return false;
	udp_recv(pcb, snmp_recv, NULL);
	if (udp_bind(pcb, IP_ANY_TYPE, 161) != ERR_OK) {
		udp_remove(pcb);
		pcb = NULL;
		return false;
	}
	snmp_traps_handle = pcb;
	return true;
}


void snmp_transport_close(void)
{
	if (!pcb)
		return;
	udp_remove(pcb);
	pcb = NULL;
	snmp_traps_handle = NULL;
}


bool snmp_transport_is_open(void)
{
	return pcb != NULL;
}


bool snmp_transport_trap(const uint8_t ip[4], const uint8_t *d, size_t n)
{
	ip_addr_t dst;
	struct pbuf *p;
	err_t e;

	IP_ADDR4(&dst, ip[0], ip[1], ip[2], ip[3]);
	if (!pcb || !netif_default || ip4_addr_isany(netif_ip4_addr(netif_default)))
		return false;
	p = pbuf_alloc(PBUF_TRANSPORT, n, PBUF_RAM);
	if (!p)
		return false;
	pbuf_take(p, d, n);
	e = udp_sendto(pcb, p, &dst, 162);
	pbuf_free(p);
	return e == ERR_OK;
}
