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
#include "events.h"
#include "mcast.h"
#include "plat.h"

extern __code const struct machine machine;

/* 01:80:C2:00:00:03: the fourth reserved-multicast register, after 00, 01 and
 * 02, as RTL8367C's RMA_CTRL03; RMA0E is the eighth (0x4ee8) */
#define RMA03_CONF		(RTL837X_RMA0_CONF + 3 * 4)
#define EAPOL_TYPE		0x888e
#define PAE_GROUP		"\x01\x80\xc2\x00\x00\x03"

static uint8_t frame[1600];

/* rtl_env.c: lay the gate over the configured isolation masks and learning limits */
void rtl_iso_gate(uint16_t gated, uint16_t learn);
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


void dot1x_hw_gate(uint16_t blocked, uint16_t learn, bool on)
{
	static uint16_t was, was_learn;
	uint16_t flush = blocked & (~was | (was_learn & ~learn));

	trap_rma_set(RMA03_CONF, on ? RTL837X_RMA_ACT_TRAP : RTL837X_RMA_ACT_FORWARD);
	rtl_iso_gate(blocked, learn);
	/* a closed port's learned addresses would still draw unicast to it */
	for (uint8_t port = 0; port < DOT1X_PORTS; port++)
		if ((flush >> port) & 1)
			port_l2_forget_port(port);
	was = blocked;
	was_learn = learn;
}


/*
 * A port put in a VLAN: an untagged member, with it as its PVID, and out of
 * the VLAN its untagged frames went to. What it was is kept where a restart
 * of ours does not clear it, as the switch's tables outlive that.
 */
#define MOVED_MAGIC	0x31784456

static struct {
	uint32_t magic;
	struct moved {
		uint16_t vlan, pvid;
		bool member, tagged;		/* of vlan, before */
		bool pmember, ptagged;		/* of pvid */
	} port[DOT1X_PORTS];
} moved __attribute__((section(".uninitialized_data.dot1x")));

/* Membership of one port; false if the VLAN has no members */
static bool vlan_member(uint16_t vlan, uint8_t port, bool *member, bool *tagged)
{
	uint16_t m, t;

	if (!mc_hw_vlan(vlan, &m, &t))
		return false;
	*member = (m >> port) & 1;
	*tagged = (t >> port) & 1;
	return true;
}


static void vlan_member_set(uint16_t vlan, uint8_t port, bool member, bool tagged)
{
	uint16_t m, t;

	if (!mc_hw_vlan(vlan, &m, &t))
		return;
	m = member ? m | 1 << port : m & ~(1 << port);
	t = member && tagged ? t | 1 << port : t & ~(1 << port);
	vlan_settings.vlan = vlan;
	vlan_settings.members = m;
	vlan_settings.tagged = t;
	vlan_create();
}


bool dot1x_hw_vlan(uint8_t port, uint16_t vlan)
{
	struct moved *v = &moved.port[port];
	bool member, tagged;

	if (moved.magic != MOVED_MAGIC) {
		memset(&moved, 0, sizeof(moved));
		moved.magic = MOVED_MAGIC;
	}
	if (v->vlan) {
		vlan_member_set(v->pvid, port, v->pmember, v->ptagged);
		port_pvid_set(port, v->pvid);
		vlan_member_set(v->vlan, port, v->member, v->tagged);
		v->vlan = 0;
		port_l2_forget_port(port);
	}
	if (!vlan)
		return true;
	if (!vlan_member(vlan, port, &member, &tagged))
		return false;
	v->pvid = port_pvid_get(port);
	if (v->pvid == vlan)
		return true;		/* its own already */
	v->member = member;
	v->tagged = tagged;
	v->pmember = v->ptagged = false;
	vlan_member(v->pvid, port, &v->pmember, &v->ptagged);
	v->vlan = vlan;
	vlan_member_set(vlan, port, true, false);
	port_pvid_set(port, vlan);
	if (v->pmember && !v->ptagged)
		vlan_member_set(v->pvid, port, false, false);
	port_l2_forget_port(port);
	return true;
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
	event(EV_DOT1X, dot1x_hw_front_port(port), 0, "%s %u", msg, dot1x_hw_front_port(port));
}


/* ---- from rtl_app.c ---- */

/* An RX frame, CPU tag in place, and the CPU port's 802.1Q tag unless it was
 * trapped (EAPOL): true if 802.1X took or dropped it */
bool dot1x_plat_rx(const uint8_t *f, uint16_t len, uint16_t type_at)
{
	uint8_t port = f[19] & 0x0f;
	uint16_t type;

	if (!dot1x_cfg.enabled || len < type_at + 2)
		return false;
	if (f[20] != 0x81 || f[21] != 0x00)
		type_at -= 4;
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


bool dot1x_plat_mab(void)
{
	for (uint8_t i = 0; i < DOT1X_PORTS; i++)
		if (dot1x_cfg.enabled && dot1x_cfg.mode[i] == DOT1X_AUTO && dot1x_cfg.mab[i])
			return true;
	return false;
}


/* LINK message: links_sts bit 16 + port (8-9: bit port), as rtl_metrics.c reads it */
static uint32_t link_sts;

bool dot1x_hw_link(uint8_t port)
{
	return (link_sts >> (port < 8 ? 16 + port : port)) & 1;
}


void dot1x_plat_links(uint32_t sts, uint32_t links, uint32_t links_89)
{
	link_sts = sts;
	for (uint8_t i = machine.min_port; i <= machine.max_port && i < DOT1X_PORTS; i++) {
		bool up = (sts >> (i < 8 ? 16 + i : i)) & 1;
		(void)links; (void)links_89;
		dot1x_link(i, up);
	}
}


/* The 8051 came up again with the registers at their defaults */
void dot1x_plat_bridge_up(void)
{
	for (uint8_t port = 0; port < DOT1X_PORTS; port++)
		if (moved.magic == MOVED_MAGIC && moved.port[port].vlan < 4095)
			dot1x_hw_vlan(port, 0);
	dot1x_init();
}


/* ---- /dot1x.json ---- */

extern __xdata uint8_t outbuf[TCP_OUTBUF_SIZE];
extern __xdata uint16_t slen;

size_t dot1x_json(char *buf, size_t max)
{
	static const char *const modes[] = { "force-auth", "auto", "force-unauth" };
	static const char *const methods[] = { "", "eap", "mab" };
	bool first = true;
	size_t n = 0;

	n = app(buf, max, n, "{\"enabled\":%d,\"radius\":\"%u.%u.%u.%u\",\"radius_port\":%u,\"secret\":%d,\"radsec\":%d,\"reauth\":%lu,\"radius_vlan\":%d,\"ports\":[",
	    dot1x_cfg.enabled, dot1x_cfg.radius_ip[0], dot1x_cfg.radius_ip[1], dot1x_cfg.radius_ip[2],
	    dot1x_cfg.radius_ip[3], dot1x_cfg.radius_port, dot1x_cfg.secret[0] != 0, dot1x_cfg.radsec, (unsigned long)dot1x_cfg.reauth_s,
	    dot1x_cfg.radius_vlan);
	for (uint8_t i = machine.min_port; i <= machine.max_port && i < DOT1X_PORTS; i++) {
		const struct dot1x_status *s = dot1x_port_status(i);
		char user[DOT1X_IDENTITY_MAX];
		uint8_t k;

		for (k = 0; s->identity[k] && k < sizeof(user) - 1; k++) {
			char c = s->identity[k];
			user[k] = c < 0x20 || c > 0x7e || c == '"' || c == '\\' ? '.' : c;
		}
		user[k] = 0;
		n = app(buf, max, n, "%s{\"port\":%u,\"mode\":\"%s\",\"mab\":%d,\"state\":\"%s\",\"authorized\":%d,\"method\":\"%s\",",
		    first ? "" : ",", machine.log_to_phys_port[i], modes[dot1x_cfg.mode[i] % 3], dot1x_cfg.mab[i],
		    dot1x_state_name(s->state), s->authorized, methods[s->method % 3]);
		char mac[18] = "";
		if (s->mac_valid)
			snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
				 s->mac[0], s->mac[1], s->mac[2], s->mac[3], s->mac[4], s->mac[5]);
		n = app(buf, max, n, "\"mac\":\"%s\",\"user\":\"%s\",\"ok\":%lu,\"fail\":%lu,\"reauth_left\":%lu,"
		    "\"vlan\":%u,\"guest\":%u,\"authfail\":%u}",
		    mac, user, (unsigned long)s->successes, (unsigned long)s->failures, (unsigned long)s->reauth_left,
		    s->vlan, dot1x_cfg.guest_vlan[i], dot1x_cfg.fail_vlan[i]);
		first = false;
	}
	return app(buf, max, n, "]}");
}


void send_dot1x(void)
{
	slen = strtox(outbuf, "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\n\r\n");
	slen += dot1x_json((char *)outbuf + slen, TCP_OUTBUF_SIZE - slen);
}
