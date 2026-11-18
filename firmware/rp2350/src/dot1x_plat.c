/*
 * dot1x.c on the switch:
 *
 * - EAPOL in: the reserved-multicast trap for 01:80:C2:00:00:03 (RMA03) sends
 *   every EAPOL frame to the CPU port while 802.1X is on; rtl_rx_frame() hands
 *   it to dot1x_eapol_rx(), and drops everything else from a gated port,
 *   noting its source MAC for MAB.
 * - EAPOL out: CPU-tagged frames to one port, as LACP sends its PDUs.
 * - The gate: port isolation (rtl_env.c keeps the configured masks and lays
 *   the gate over them). A gated port forwards to the CPU port only, and no
 *   other port forwards to it; its learned addresses are flushed.
 * - RADIUS: a UDP socket on lwIP, in dot1x_udp.c (compiled without the shim).
 *
 * Compiled with sdcc_shim.h, like the modules, for the machine description.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_port.h"
#include "rtl837x_trap.h"
#include "machine.h"
#include "uip/uip.h"

#include "dot1x.h"
#include "dot1x_plat.h"
#include "plat.h"

extern __code const struct machine machine;

/* 01:80:C2:00:00:03: the fourth reserved-multicast register, after 00, 01 and
 * 02, as RTL8367C's RMA_CTRL03; RMA0E is the eighth (0x4ee8) */
#define RMA03_CONF		(RTL837X_RMA0_CONF + 3 * 4)
#define EAPOL_TYPE		0x888e
#define PAE_GROUP		"\x01\x80\xc2\x00\x00\x03"

static uint8_t frame[1600];

/* rtl_env.c: lay the gate over the configured isolation masks */
void rtl_iso_gate(uint16_t gated);
const uint8_t *rtl_mac(void);


uint16_t dot1x_hw_ports(void)
{
	uint16_t m = 0;

	for (uint8_t i = machine.min_port; i <= machine.max_port && i < DOT1X_PORTS; i++)
		m |= 1 << i;
	return m;
}


uint8_t dot1x_hw_front_port(uint8_t port)
{
	return machine.log_to_phys_port[port];
}


void dot1x_hw_eapol_tx(uint8_t port, const uint8_t *pdu, uint16_t len)
{
	uint16_t n = 12 + RTL_TAG_SIZE + 2 + len;

	if (n > sizeof(frame))
		return;
	memcpy(frame, PAE_GROUP, 6);
	memcpy(frame + 6, rtl_mac(), 6);
	frame[12] = RTL_FRAME_TAG_ID >> 8;
	frame[13] = RTL_FRAME_TAG_ID & 0xff;
	frame[14] = RTL_FRAME_TAG_VERSION;
	frame[15] = 0;
	frame[16] = (RTL_TAG_LEARN_DIS | RTL_TAG_KEEP) >> 8;
	frame[17] = (RTL_TAG_LEARN_DIS | RTL_TAG_KEEP) & 0xff;
	frame[18] = (1 << port) >> 8;
	frame[19] = (1 << port) & 0xff;
	frame[20] = EAPOL_TYPE >> 8;
	frame[21] = EAPOL_TYPE & 0xff;
	memcpy(frame + 22, pdu, len);
	/* the shortest Ethernet frame, behind the CPU tag */
	if (n < 60 + RTL_TAG_SIZE) {
		memset(frame + n, 0, 60 + RTL_TAG_SIZE - n);
		n = 60 + RTL_TAG_SIZE;
	}
	rtl_tx_frame(frame, n);
}


void dot1x_hw_gate(uint16_t blocked, bool on)
{
	static uint16_t was;
	uint16_t newly = blocked & ~was;

	trap_rma_set(RMA03_CONF, on ? RTL837X_RMA_ACT_TRAP : RTL837X_RMA_ACT_FORWARD);
	rtl_iso_gate(blocked);
	/* a closed port's learned addresses would still draw unicast to it */
	for (uint8_t port = 0; port < DOT1X_PORTS; port++)
		if ((newly >> port) & 1)
			port_l2_forget_port(port);
	was = blocked;
}


void dot1x_hw_random(uint8_t *p, uint8_t n)
{
	while (n) {
		uint32_t r = plat_random32();
		for (uint8_t k = 0; k < 4 && n; k++, n--)
			*p++ = r >> (8 * k);
	}
}


void dot1x_hw_nas_ip(uint8_t ip[4])
{
	memcpy(ip, uip_hostaddr, 4);
}


const uint8_t *dot1x_hw_mac(void)
{
	return rtl_mac();
}


void dot1x_hw_log(const char *msg, uint8_t port)
{
	print_string(msg);
	write_char(' ');
	itoa(dot1x_hw_front_port(port));
	write_char('\n');
}


/* ---- from rtl_app.c ---- */

/* An RX frame, CPU tag and 802.1Q tag in place: true if 802.1X took or dropped it */
bool dot1x_plat_rx(const uint8_t *f, uint16_t len, uint16_t type_at)
{
	uint8_t port = f[19] & 0x0f;
	uint16_t type;

	if (!dot1x_cfg.enabled || len < type_at + 2)
		return false;
	type = f[type_at] << 8 | f[type_at + 1];
	if (type == EAPOL_TYPE) {
		dot1x_eapol_rx(port, f + 6, f + type_at + 2, len - type_at - 2);
		return true;
	}
	if ((dot1x_blocked() >> port) & 1) {
		dot1x_seen(port, f + 6);
		return true;
	}
	return false;
}


/* LINK message: links_sts bit 16 + port (8-9: bit port), as rtl_metrics.c reads it */
void dot1x_plat_links(uint32_t sts, uint32_t links, uint32_t links_89)
{
	for (uint8_t i = machine.min_port; i <= machine.max_port && i < DOT1X_PORTS; i++) {
		bool up = (sts >> (i < 8 ? 16 + i : i)) & 1;
		(void)links; (void)links_89;
		dot1x_link(i, up);
	}
}


/* The 8051 came up again with the registers at their defaults */
void dot1x_plat_bridge_up(void)
{
	dot1x_init();
}


/* ---- /dot1x.json ---- */

extern __xdata uint8_t outbuf[TCP_OUTBUF_SIZE];
extern __xdata uint16_t slen;

static void out(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf((char *)outbuf + slen, TCP_OUTBUF_SIZE - slen, fmt, ap);
	va_end(ap);
	if (n > 0)
		slen = slen + n < TCP_OUTBUF_SIZE ? slen + n : TCP_OUTBUF_SIZE - 1;
}


void send_dot1x(void)
{
	static const char *const modes[] = { "force-auth", "auto", "force-unauth" };
	static const char *const methods[] = { "", "eap", "mab" };
	bool first = true;

	slen = 0;
	out("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\n\r\n");
	out("{\"enabled\":%d,\"radius\":\"%u.%u.%u.%u\",\"radius_port\":%u,\"secret\":%d,\"reauth\":%lu,\"ports\":[",
	    dot1x_cfg.enabled, dot1x_cfg.radius_ip[0], dot1x_cfg.radius_ip[1], dot1x_cfg.radius_ip[2],
	    dot1x_cfg.radius_ip[3], dot1x_cfg.radius_port, dot1x_cfg.secret[0] != 0, (unsigned long)dot1x_cfg.reauth_s);
	for (uint8_t i = machine.min_port; i <= machine.max_port && i < DOT1X_PORTS; i++) {
		const struct dot1x_status *s = dot1x_port_status(i);
		char user[DOT1X_IDENTITY_MAX];
		uint8_t k;

		for (k = 0; s->identity[k] && k < sizeof(user) - 1; k++) {
			char c = s->identity[k];
			user[k] = c < 0x20 || c > 0x7e || c == '"' || c == '\\' ? '.' : c;
		}
		user[k] = 0;
		out("%s{\"port\":%u,\"mode\":\"%s\",\"mab\":%d,\"state\":\"%s\",\"authorized\":%d,\"method\":\"%s\",",
		    first ? "" : ",", machine.log_to_phys_port[i], modes[dot1x_cfg.mode[i] % 3], dot1x_cfg.mab[i],
		    dot1x_state_name(s->state), s->authorized, methods[s->method % 3]);
		char mac[18] = "";
		if (s->mac_valid)
			snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
				 s->mac[0], s->mac[1], s->mac[2], s->mac[3], s->mac[4], s->mac[5]);
		out("\"mac\":\"%s\",\"user\":\"%s\",\"ok\":%lu,\"fail\":%lu,\"reauth_left\":%lu}",
		    mac, user, (unsigned long)s->successes, (unsigned long)s->failures, (unsigned long)s->reauth_left);
		first = false;
	}
	out("]}");
}
