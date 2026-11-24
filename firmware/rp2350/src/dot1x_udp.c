/*
 * dot1x.c's RADIUS socket, on lwIP: one UDP socket on an ephemeral port,
 * opened at the first request. Replies from anyone but the configured server
 * are dropped; dot1x.c checks the rest.
 */

#include <string.h>

#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include "dot1x.h"
#include "radsec.h"

static struct udp_pcb *pcb;

static void radius_recv(void *arg, struct udp_pcb *upcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
	static uint8_t buf[1800];
	uint16_t n;

	(void)arg; (void)upcb; (void)port;
	ip_addr_t server;

	IP_ADDR4(&server, dot1x_cfg.radius_ip[0], dot1x_cfg.radius_ip[1], dot1x_cfg.radius_ip[2], dot1x_cfg.radius_ip[3]);
	if (!radsec_on() && ip_addr_cmp(addr, &server)) {
		n = pbuf_copy_partial(p, buf, sizeof(buf), 0);
		dot1x_radius_rx(buf, n);
	}
	pbuf_free(p);
}


void dot1x_hw_radius_tx(const uint8_t *p, uint16_t len)
{
	ip_addr_t dst;
	struct pbuf *b;

	if (radsec_on()) {
		radsec_tx(p, len);
		return;
	}
	if (!pcb) {
		pcb = udp_new_ip_type(IPADDR_TYPE_V4);
		if (!pcb)
			return;
		udp_recv(pcb, radius_recv, NULL);
		if (udp_bind(pcb, IP_ANY_TYPE, 0) != ERR_OK) {
			udp_remove(pcb);
			pcb = NULL;
			return;
		}
	}
	b = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
	if (!b)
		return;
	memcpy(b->payload, p, len);
	IP_ADDR4(&dst, dot1x_cfg.radius_ip[0], dot1x_cfg.radius_ip[1], dot1x_cfg.radius_ip[2], dot1x_cfg.radius_ip[3]);
	udp_sendto(pcb, b, &dst, dot1x_cfg.radius_port);
	pbuf_free(b);
}


