/*
 * The slice of uIP's UDP API that RTLPlayground's syslog.c, dns.c and ntp.c
 * use, so they compile unchanged: connections from uip_udp_new(), polled
 * and fed through the same callbacks as on the 8051. udp_lwip.c moves the
 * datagrams through lwIP. Compiled with sdcc_shim.h, like the modules.
 */

#include "rtl837x_common.h"
#include "uip/uip.h"
#include "dhcp.h"
#include "dns.h"
#include "ntp.h"
#include "syslog.h"

#include "plat.h"

#if RTL_UDP_CONNS > UIP_UDP_CONNS
#error "more UDP connections than uIP's table holds"
#endif

__xdata struct uip_udp_conn uip_udp_conns[UIP_UDP_CONNS];
__xdata struct uip_udp_conn * __xdata uip_udp_conn;
__xdata void * __xdata uip_appdata;
__xdata u8_t uip_flags;
__xdata u16_t uip_slen;

static uint8_t udp_buf[RTL_UDP_BUF];
static uint16_t lastport = 4096;


__xdata struct uip_udp_conn *uip_udp_new(__xdata uip_ipaddr_t * __xdata ripaddr, __xdata u16_t rport)
{
	struct uip_udp_conn *c = 0;
	uint16_t port;

	/* Pick a local port no live connection uses, as uIP does */
again:
	if (++lastport >= 32000)
		lastport = 4096;
	port = HTONS(lastport);
	for (int i = 0; i < RTL_UDP_CONNS; i++)
		if (uip_udp_conns[i].lport == port)
			goto again;

	for (int i = 0; i < RTL_UDP_CONNS; i++) {
		if (!uip_udp_conns[i].lport) {
			c = &uip_udp_conns[i];
			break;
		}
	}
	if (!c)
		return 0;
	c->lport = port;
	c->rport = rport;
	if (ripaddr)
		uip_ipaddr_copy(&c->ripaddr, ripaddr);
	else
		memset(&c->ripaddr, 0, sizeof(c->ripaddr));
	c->ttl = 64;
	return c;
}


void uip_send(__xdata const void *data, __xdata uint16_t len)
{
	if (len > RTL_UDP_BUF)
		len = RTL_UDP_BUF;
	if (data != udp_buf)
		memmove(udp_buf, data, len);
	uip_slen = len;
}


void rtl_udp_info(int i, struct rtl_udp_info *info)
{
	struct uip_udp_conn *c = &uip_udp_conns[i];

	info->lport = HTONS(c->lport);
	info->rport = HTONS(c->rport);
	memcpy(info->ripaddr, &c->ripaddr, 4);
}


/* As udp_apps.c; DHCP is lwIP's, so dhcp_callback() has no part here */
static void callbacks(int i)
{
	uip_udp_conn = &uip_udp_conns[i];
	uip_appdata = udp_buf;
	uip_slen = 0;
	syslog_callback(uip_udp_conn->lport);
	dns_callback(uip_udp_conn->lport);
	ntp_callback(uip_udp_conn->lport);
}


uint16_t rtl_udp_poll(int i, uint8_t **data)
{
	if (!uip_udp_conns[i].lport)
		return 0;
	uip_flags = UIP_POLL;
	uip_len = 0;
	callbacks(i);
	uip_flags = 0;
	*data = udp_buf;
	return uip_slen;
}


uint16_t rtl_udp_input(int i, const uint8_t *p, uint16_t len, uint8_t **data)
{
	if (!uip_udp_conns[i].lport)
		return 0;
	if (len > RTL_UDP_BUF)
		len = RTL_UDP_BUF;
	memcpy(udp_buf, p, len);
	uip_flags = UIP_NEWDATA;
	uip_len = len;
	callbacks(i);
	uip_flags = 0;
	uip_len = 0;
	*data = udp_buf;
	return uip_slen;
}
