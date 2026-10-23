/*
 * What the dashboard and the SNMP agent read from the switch: the ports
 * with their counters (one STAT_READ for all of them), the chip's
 * temperature, the MAC table's size, the SFP modules' diagnostics, a
 * timeline of link changes, and what STP, LACP and IGMP snooping are
 * doing. Compiled with sdcc_shim.h, like the modules.
 */

#include <stdio.h>

#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl837x_port.h"
#include "rtl837x_pins.h"
#include "rtl837x_lacp.h"
#include "machine.h"
#include "phy.h"
#include "sfp.h"

#include "bridge.h"
#include "plat.h"
#include "rtl_metrics.h"

#define LINK_EVENTS		32
#define IGMP_GROUPS		64
#define IGMP_MEMBERSHIP_S	260	/* RFC 2236's group membership interval */

extern __code const struct machine machine;
extern __xdata uint8_t sfr_data[4];
extern __xdata char port_names[9][PORT_NAME_SIZE];
extern __xdata bool stp_enabled;
extern __xdata uint8_t lacpEnabled;
extern __xdata uint8_t igmpEnabled;
extern __xdata uint16_t stp_tc_count;
extern __xdata uint8_t stp_root_port;
extern __xdata uint8_t lacp_actor_state[10];
extern __xdata uint8_t lacp_partner_state[10];
extern __xdata uint8_t lacp_partner_sys[10][MAC_ADDR_LEN];
extern __xdata uint16_t lacp_partner_key[10];

/* The counters, by number in the ASIC's MIB (the web UI's table). The
 * 32-bit ones come in pairs in one 64-bit word, the first one high. */
static __code const uint8_t stat_ids[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 48 };
static bool stat_read_unsupported;

static struct rtl_link_event events[LINK_EVENTS];
static uint8_t ev_next;
static uint32_t ev_seq;
static uint8_t link_last[RTL_MAX_PORTS + 1];	/* by logical port */
static bool link_seen;

static uint32_t stp_root_changes;
static uint8_t stp_root_last = 0xff;
static uint32_t lacp_changes;
static uint8_t lacp_last[10][MAC_ADDR_LEN + 3];

static struct {
	uint8_t ip[4];
	uint16_t ports;
	uint32_t seen_s;
} igmp[IGMP_GROUPS];
static uint32_t igmp_joins, igmp_leaves;

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | p[2] << 8 | p[3];
}


static uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}


static uint32_t reg32(uint16_t reg)
{
	reg_read_m(reg);
	return be32(sfr_data);
}


/* As send_status(): a link bit, then a speed nibble per logical port */
static uint8_t link_code(uint8_t i, uint32_t sts, uint32_t links, uint32_t links_89)
{
	if (!((sts >> (i < 8 ? 16 + i : i)) & 1))
		return 0;
	return (((i < 8 ? links : links_89) >> (4 * (i & 7))) & 0xf) + 1;
}


static void stat_fallback(uint8_t i, uint64_t *v)
{
	for (uint8_t k = 0; k < sizeof(stat_ids); k++) {
		uint64_t hi;
		STAT_GET(stat_ids[k], i);
		hi = reg32(RTL837X_STAT_V_HIGH);
		v[k] = hi << 32 | reg32(RTL837X_STAT_V_LOW);
	}
}


uint8_t rtl_ports_read(struct rtl_port *p, uint8_t max)
{
	static uint8_t r[RTL_MAX_PORTS * sizeof(stat_ids) * 8];
	uint64_t v[RTL_MAX_PORTS][sizeof(stat_ids)];
	uint8_t n = rtl_port_count(), log[RTL_MAX_PORTS];
	uint16_t pmask = 0;
	size_t got;

	if (n > max)
		n = max;
	if (n > RTL_MAX_PORTS)
		n = RTL_MAX_PORTS;
	for (uint8_t k = 0; k < n; k++) {
		log[k] = machine.phys_to_log_port[k];
		pmask |= 1 << log[k];
	}

	/* STAT_READ answers in logical port order */
	uint8_t q[3 + sizeof(stat_ids)] = { pmask, pmask >> 8, sizeof(stat_ids) };
	memcpy(q + 3, stat_ids, sizeof(stat_ids));
	int st = stat_read_unsupported ? ST_UNKNOWN
		: bridge_request(REQ_STAT_READ, q, sizeof(q), NULL, 0, r, sizeof(r), &got);
	if (st == ST_UNKNOWN)
		stat_read_unsupported = true;
	for (uint8_t k = 0; k < n; k++) {
		if (st == ST_OK && got == (size_t)n * sizeof(stat_ids) * 8) {
			uint8_t idx = 0;
			for (uint8_t j = 0; j < n; j++)
				idx += log[j] < log[k];
			for (uint8_t c = 0; c < sizeof(stat_ids); c++) {
				const uint8_t *b = r + (idx * sizeof(stat_ids) + c) * 8;
				v[k][c] = (uint64_t)le32(b + 4) << 32 | le32(b);
			}
		} else if (st == ST_UNKNOWN) {
			stat_fallback(log[k], v[k]);
		} else {
			memset(v[k], 0, sizeof(v[k]));
		}
	}

	uint32_t sts = reg32(RTL837X_REG_LINKS_STS);
	uint32_t links = reg32(RTL837X_REG_LINKS);
	uint32_t links_89 = reg32(RTL837X_REG_LINKS_89);
	for (uint8_t k = 0; k < n; k++) {
		uint8_t i = log[k];
		struct rtl_port *o = &p[k];

		memset(o, 0, sizeof(*o));
		if (port_names[i][0])
			strncpy(o->name, port_names[i], sizeof(o->name) - 1);
		else
			snprintf(o->name, sizeof(o->name), "Port %u", k + 1);
		o->sfp = machine.is_sfp[i] != 0;
		if (o->sfp) {
			o->enabled = true;
		} else {
			phy_read(i, PHY_MMD31, 0xa610);
			o->enabled = SFR_DATA_8 == 0x20;
		}
		o->link = link_code(i, sts, links, links_89);
		o->in_octets = v[k][0];
		o->out_octets = v[k][1];
		o->in_ucast = v[k][2];
		o->in_mcast = v[k][3];
		o->in_bcast = v[k][4];
		o->out_ucast = v[k][5];
		o->out_mcast = v[k][6];
		o->out_bcast = v[k][7];
		o->out_discards = v[k][8] >> 32;
		o->in_discards = v[k][8];
		o->in_errors = v[k][9] >> 32;
		o->out_errors = v[k][9];
	}
	return n;
}


void rtl_status_read(struct rtl_status *s)
{
	memset(s, 0, sizeof(*s));
	reg_read(RTL837X_TM_RESULT);
	s->chip_temp = (int16_t)SFR_DATA_U16;
	for (uint8_t i = machine.min_port; i <= machine.max_port; i++) {
		reg_read(RTL837X_L2_LRN_PORT_CONSTRT_CNT + (i << 2));
		s->mac_entries += SFR_DATA_U16 & L2_LRN_PORT_CNT_MASK;
	}
	s->stp_on = stp_enabled;
	s->stp_root_port = stp_root_port;
	s->stp_tc = stp_tc_count;
	s->stp_root_changes = stp_root_changes;
	s->lacp_on = lacpEnabled;
	for (uint8_t i = 0; i < 10; i++)
		if ((lacp_actor_state[i] & LACP_STATE_FULL) == LACP_STATE_FULL)
			s->lacp_ports_up++;
	s->lacp_changes = lacp_changes;
	s->igmp_on = igmpEnabled;
	for (uint8_t i = 0; i < IGMP_GROUPS; i++)
		s->igmp_groups += igmp[i].ports != 0;
	s->igmp_joins = igmp_joins;
	s->igmp_leaves = igmp_leaves;
}


uint8_t rtl_sfp_count(void)
{
	return machine.n_sfp;
}


void rtl_sfp_read(uint8_t slot, struct rtl_sfp *d)
{
	memset(d, 0, sizeof(*d));
	if (slot >= machine.n_sfp)
		return;
	d->present = !(sfp_pins_last & (0x1 << (slot << 2)));
	d->los = !!(sfp_pins_last & (0x2 << (slot << 2)));
	if (!d->present)
		return;
	strncpy(d->vendor, sfp_module_vendor[slot], sizeof(d->vendor) - 1);
	strncpy(d->model, sfp_module_model[slot], sizeof(d->model) - 1);
	/* SFF-8472: byte 92 bit 6, diagnostics implemented; A2h bytes 96-105 */
	if (!(sfp_options[slot] & 0x40) || !sfp_read_block(slot, 0x80 | 96, 10))
		return;
	d->ddm = true;
	d->temp = (int16_t)(sfp_buf[0] << 8 | sfp_buf[1]);
	d->vcc = sfp_buf[2] << 8 | sfp_buf[3];
	d->bias = sfp_buf[4] << 8 | sfp_buf[5];
	d->tx_power = sfp_buf[6] << 8 | sfp_buf[7];
	d->rx_power = sfp_buf[8] << 8 | sfp_buf[9];
}


uint8_t rtl_link_events(struct rtl_link_event *out, uint8_t max)
{
	uint8_t n = 0;

	for (uint8_t k = 0; k < LINK_EVENTS && n < max; k++) {
		struct rtl_link_event *e = &events[(ev_next + k) % LINK_EVENTS];
		if (e->seq)
			out[n++] = *e;
	}
	return n;
}


/* From MSG_LINK: a timeline entry for each port whose link changed */
void rtl_metrics_link(uint32_t sts, uint32_t links, uint32_t links_89)
{
	for (uint8_t i = machine.min_port; i <= machine.max_port && i <= RTL_MAX_PORTS; i++) {
		uint8_t now = link_code(i, sts, links, links_89);

		if (link_seen && now == link_last[i])
			continue;
		link_last[i] = now;
		if (!link_seen && !now)
			continue;	/* down at the first report: nothing happened */
		struct rtl_link_event *e = &events[ev_next];
		e->seq = ++ev_seq;
		e->t_ms = plat_ms();
		e->port = machine.log_to_phys_port[i];
		e->link = now;
		ev_next = (ev_next + 1) % LINK_EVENTS;
	}
	link_seen = true;
}


/* Once a second: root port and LACP partner changes, IGMP membership ageing */
void rtl_metrics_second(void)
{
	if (stp_enabled && stp_root_port != stp_root_last)
		stp_root_changes++;
	stp_root_last = stp_root_port;

	for (uint8_t i = 0; i < 10; i++) {
		uint8_t now[MAC_ADDR_LEN + 3];
		memcpy(now, lacp_partner_sys[i], MAC_ADDR_LEN);
		now[MAC_ADDR_LEN] = lacp_partner_key[i] >> 8;
		now[MAC_ADDR_LEN + 1] = lacp_partner_key[i];
		now[MAC_ADDR_LEN + 2] = lacp_partner_state[i] & LACP_STATE_SYNC;
		if (lacpEnabled && memcmp(now, lacp_last[i], sizeof(now)))
			lacp_changes++;
		memcpy(lacp_last[i], now, sizeof(now));
	}

	uint32_t t = plat_uptime_s();
	for (uint8_t i = 0; i < IGMP_GROUPS; i++)
		if (igmp[i].ports && t - igmp[i].seen_s > IGMP_MEMBERSHIP_S)
			igmp[i].ports = 0;
}


static void igmp_member(const uint8_t *ip, uint8_t port, bool join)
{
	int free = -1;

	for (int i = 0; i < IGMP_GROUPS; i++) {
		if (igmp[i].ports && !memcmp(igmp[i].ip, ip, 4)) {
			if (join) {
				igmp[i].ports |= 1 << port;
				igmp[i].seen_s = plat_uptime_s();
			} else {
				igmp[i].ports &= ~(1 << port);
			}
			return;
		}
		if (!igmp[i].ports && free < 0)
			free = i;
	}
	if (join && free >= 0) {
		memcpy(igmp[free].ip, ip, 4);
		igmp[free].ports = 1 << port;
		igmp[free].seen_s = plat_uptime_s();
	}
}


/*
 * An IGMP frame trapped to the CPU, while snooping is on: membership as
 * the reports and leaves say, per port. The IPv4 header follows the CPU
 * tag, with or without an 802.1Q tag in between.
 */
void rtl_metrics_igmp(const uint8_t *f, uint16_t len)
{
	uint16_t ip;
	uint8_t port = f[19] & 0xf;	/* the CPU tag's port field */

	if (len > 22 && f[20] == 0x08 && f[21] == 0x00)
		ip = 22;
	else if (len > 26 && f[24] == 0x08 && f[25] == 0x00)
		ip = 26;
	else
		return;
	if (f[ip + 9] != 2 || port > 15)
		return;
	uint16_t g = ip + ((f[ip] & 0x0f) << 2);
	if (g + 8 > len)
		return;
	switch (f[g]) {
	case 0x12:		/* v1 report */
	case 0x16:		/* v2 report */
		igmp_joins++;
		igmp_member(f + g + 4, port, true);
		break;
	case 0x17:		/* v2 leave */
		igmp_leaves++;
		igmp_member(f + g + 4, port, false);
		break;
	case 0x22: {		/* v3 report: each group record */
		uint16_t n = f[g + 6] << 8 | f[g + 7], r = g + 8;
		while (n-- && r + 8 <= len) {
			uint8_t type = f[r];
			uint16_t srcs = f[r + 2] << 8 | f[r + 3];
			/* EXCLUDE: all sources, a member; INCLUDE nothing: gone */
			if (type == 2 || type == 4) {
				igmp_joins++;
				igmp_member(f + r + 4, port, true);
			} else if ((type == 1 || type == 3) && !srcs) {
				igmp_leaves++;
				igmp_member(f + r + 4, port, false);
			} else if (type == 1 || type == 3 || type == 5) {
				igmp_joins++;
				igmp_member(f + r + 4, port, true);
			}
			r += 8 + f[r + 1] * 4 + srcs * 4;
		}
		break;
	}
	}
}
