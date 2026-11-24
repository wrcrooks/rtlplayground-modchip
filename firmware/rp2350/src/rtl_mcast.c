/*
 * mcast.c on the switch. Compiled with sdcc_shim.h, like the modules.
 *
 *   mcsnoop                           status
 *   mcsnoop on|off                    IGMP and MLD snooping per VLAN
 *   mcsnoop querier on|off
 *   mcsnoop interval <10-3600>        s between queries, 125 at first
 *   mcsnoop unknown flood|router      IPv4 multicast nobody asked for
 *   mcsnoop router <n> on|off         a port to a multicast router
 *
 * It replaces RTLPlayground's "igmp on", which knows no VLANs: the
 * one turned on last replaces the other.
 */

#include <stdio.h>
#include <string.h>

#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl837x_port.h"
#include "rtl837x_igmp.h"
#include "machine.h"
#include "cmd_parser.h"
#include "uip/uip.h"

#include "events.h"
#include "mcast.h"
#include "plat.h"
#include "rtl_ext.h"

extern __code const struct machine machine;
extern __xdata struct machine_runtime machine_detected;
extern __xdata uint8_t sfr_data[4];
extern __xdata uint8_t igmpEnabled;

const uint8_t *rtl_mac(void);

#define MLD_TRAP	0x00000280	/* IGMP_TRAP for MLDv1 and v2, bits 6-9 */
#define CPU_MASK	0x200

static uint32_t up_sts;
static bool ipv6_to_cpu;

uint16_t mc_hw_ports(void)
{
	uint16_t m = 0;

	for (uint8_t i = machine.min_port; i <= machine.max_port && i < MC_PORTS; i++)
		m |= 1 << i;
	return m;
}


uint16_t mc_hw_up(void)
{
	uint16_t m = 0;

	for (uint8_t i = 0; i < MC_PORTS; i++)
		if ((up_sts >> (i < 8 ? 16 + i : i)) & 1)
			m |= 1 << i;
	return m;
}


uint8_t mc_hw_front(uint8_t port)
{
	return machine.log_to_phys_port[port];
}


uint16_t mc_hw_lag(uint8_t port)
{
	static uint16_t lags[4];
	static uint32_t read_at = UINT32_MAX;

	/* asked several times per report */
	if (read_at != plat_uptime_s()) {
		read_at = plat_uptime_s();
		for (uint8_t i = 0; i < 4; i++)
			lags[i] = port_lag_members_get(i) & 0x3ff;
	}
	for (uint8_t i = 0; i < 4; i++)
		if ((lags[i] >> port) & 1)
			return lags[i];
	return 1 << port;
}


/* vlan_create()'s layout: members in bits 0-9, untagged members in 10-19 */
bool mc_hw_vlan(uint16_t vlan, uint16_t *members, uint16_t *tagged)
{
	uint32_t v, untagged;

	if (vlan_get(vlan))
		return false;
	v = (uint32_t)sfr_data[0] << 24 | (uint32_t)sfr_data[1] << 16 | sfr_data[2] << 8 | sfr_data[3];
	*members = v & 0x3ff;
	untagged = (v >> 10) & 0x3ff;
	*tagged = *members & ~untagged;
	return *members != 0;
}


unsigned mc_hw_vlans(uint16_t *list, unsigned max)
{
	unsigned n = 0, k;

	for (uint8_t i = machine.min_port; i <= machine.max_port && i < MC_PORTS && n < max; i++) {
		uint16_t v = port_pvid_get(i);

		for (k = 0; k < n && list[k] != v; k++)
			;
		if (k == n && v)
			list[n++] = v;
	}
	return n;
}


static void table_wait(void)
{
	do {
		reg_read_m(RTL837X_TBL_CTRL);
	} while (sfr_data[3] & TBL_EXECUTE);
}


static void table_by_key(void)
{
	reg_read_m(RTL837x_TBL_DATA_0);
	sfr_data[2] &= 0x3f;	/* look up by key, not by index */
	sfr_data[1] &= 0xf8;	/* no CLEAR */
	reg_write_m(RTL837x_TBL_DATA_0);
}


/* An L2 multicast entry, as port_l2mc_set() writes them */
void mc_hw_entry(uint16_t vlan, const uint8_t *mac, uint16_t ports)
{
	uint16_t idx;

	table_wait();
	REG_WRITE(RTL837x_TBL_DATA_IN_A, mac[2], mac[3], mac[4], mac[5]);
	REG_WRITE(RTL837x_TBL_DATA_IN_B, 0x20 | (vlan >> 8) | ((ports & 0x3) << 6), vlan & 0xff, mac[0], mac[1]);
	REG_WRITE(RTL837x_TBL_DATA_IN_C, 0, 0, 0, ports >> 2);
	table_by_key();
	if (ports) {
		REG_WRITE(RTL837X_TBL_CTRL, 0, 0, TBL_L2_UNICAST, TBL_WRITE | TBL_EXECUTE);
		table_wait();
		return;
	}
	REG_WRITE(RTL837X_TBL_CTRL, 0, 0, TBL_L2_UNICAST, TBL_EXECUTE);
	table_wait();
	reg_read_m(RTL837x_TBL_DATA_0);
	if (!(sfr_data[2] & 0x10))
		return;		/* not there */
	idx = (sfr_data[2] & 0x0f) << 8 | sfr_data[3];
	sfr_data[1] |= 0x04;
	reg_write_m(RTL837x_TBL_DATA_0);
	REG_WRITE(RTL837X_TBL_CTRL, idx >> 8, idx & 0xff, TBL_L2_UNICAST, TBL_WRITE | TBL_EXECUTE);
	table_wait();
	table_by_key();
}


static void flood6(void)
{
	uint16_t all = machine_detected.isRTL8373 ? PMASK_9 : PMASK_6;

	if (ipv6_to_cpu)
		all |= CPU_MASK;
	REG_SET(RTL837X_IPV6_UNKN_MC_FLD_PMSK, all);
}


void mc_hw_mode(bool on, bool restrict_unknown, uint16_t routers)
{
	if (!on) {
		igmp_setup();
		flood6();
		return;
	}
	igmpEnabled = 0;
	reg_bit_clear(RTL837X_L2_CTRL, L2_CTRL_LUT_IPMC_HASH);	/* IPv4 multicast by MAC and VLAN */
	REG_SET(RTL837X_IGMP_TRAP_CFG, IGMP_CPU_PORT | IGMP_TRAP_PRIORITY);
	REG_SET(RTL837X_IPV4_PORT_MC_LM_ACT, LOOKUP_MISS_FLOOD);
	REG_SET(RTL837X_IPV6_PORT_MC_LM_ACT, LOOKUP_MISS_FLOOD);
	if (restrict_unknown)
		REG_SET(RTL837X_IPV4_UNKN_MC_FLD_PMSK, routers);
	else
		REG_SET(RTL837X_IPV4_UNKN_MC_FLD_PMSK, machine_detected.isRTL8373 ? PMASK_9 : PMASK_6);
	flood6();
	for (uint8_t i = machine.min_port; i <= machine.max_port; i++)
		REG_SET(RTL837X_IGMP_PORT_CFG + (i << 2), IGMP_MAX_GROUP | IGMP_PROTOCOL_ENABLE | IGMP_TRAP | MLD_TRAP);
}


/* IPv6 management: neighbor discovery is multicast the CPU must see too */
void rtl_mcast_ipv6_cpu(bool on)
{
	ipv6_to_cpu = on;
	flood6();
}


void mc_hw_tx(uint16_t ports, uint16_t vlan, bool tagged, const uint8_t *f, uint16_t len)
{
	static uint8_t b[1600];
	uint16_t n = 12;

	if (len < 14 || len > 1514)
		return;
	memcpy(b, f, 12);
	b[n++] = RTL_FRAME_TAG_ID >> 8;
	b[n++] = RTL_FRAME_TAG_ID & 0xff;
	b[n++] = RTL_FRAME_TAG_VERSION;
	b[n++] = 0;
	b[n++] = (RTL_TAG_LEARN_DIS | RTL_TAG_KEEP) >> 8;
	b[n++] = (RTL_TAG_LEARN_DIS | RTL_TAG_KEEP) & 0xff;
	b[n++] = ports >> 8;
	b[n++] = ports & 0xff;
	if (tagged) {
		b[n++] = 0x81;
		b[n++] = 0x00;
		b[n++] = vlan >> 8;
		b[n++] = vlan & 0xff;
	}
	memcpy(b + n, f + 12, len - 12);
	n += len - 12;
	if (n < 60 + RTL_TAG_SIZE) {
		memset(b + n, 0, 60 + RTL_TAG_SIZE - n);
		n = 60 + RTL_TAG_SIZE;
	}
	rtl_tx_frame(b, n);
}


const uint8_t *mc_hw_mac(void)
{
	return rtl_mac();
}


const uint8_t *mc_hw_ip(void)
{
	return (const uint8_t *)uip_hostaddr;
}


/* LINK message, as rtl_metrics.c reads it */
void rtl_mcast_links(uint32_t sts)
{
	uint16_t was = mc_hw_up(), now;

	up_sts = sts;
	now = mc_hw_up();
	for (uint8_t i = 0; i < MC_PORTS; i++)
		if (((was & ~now) >> i) & 1)
			mc_port_down(i);
}


void rtl_mcast_second(void)
{
	if (mc_cfg.on && igmpEnabled) {
		event(EV_CONFIG, 0, 0, "igmp on: mcsnoop is off");
		mc_cfg.on = false;
		mc_apply();
		igmp_enable();
	}
	mc_second();
}


static bool on_off(uint8_t w, bool *v)
{
	if (cmd_words_len != w + 1 || (!cmd_compare(w, "on") && !cmd_compare(w, "off")))
		return false;
	*v = cmd_compare(w, "on");
	return true;
}


bool rtl_mcast_cmd(void)
{
	char b[120];
	unsigned v;
	bool o;

	if (!cmd_compare(0, "mcsnoop"))
		return false;
	if (cmd_words_len == 1) {
		snprintf(b, sizeof(b), "Multicast snooping %s, querier %s, interval %u s, unknown %s, %u groups\n",
			 mc_cfg.on ? "on" : "off", mc_cfg.querier ? "on" : "off", mc_cfg.interval,
			 mc_cfg.restrict_unknown ? "to router ports" : "flooded", mc_group_count());
		print_string_x(b);
		return true;
	}
	if (on_off(1, &o)) {
		mc_cfg.on = o;
	} else if (cmd_compare(1, "querier") && on_off(2, &o)) {
		mc_cfg.querier = o;
	} else if (cmd_words_len == 3 && cmd_compare(1, "interval") && ext_word_num(2, 10, 3600, &v)) {
		mc_cfg.interval = v;
	} else if (cmd_words_len == 3 && cmd_compare(1, "unknown") && (cmd_compare(2, "flood") || cmd_compare(2, "router"))) {
		mc_cfg.restrict_unknown = cmd_compare(2, "router");
	} else if (cmd_words_len == 4 && cmd_compare(1, "router") && cmd_parse_port_separator(cmd_words_b[2])
		   && on_off(3, &o)) {
		mc_cfg.routers = o ? mc_cfg.routers | 1 << atoi_results_u8 : mc_cfg.routers & ~(1 << atoi_results_u8);
	} else {
		ext_error("mcsnoop [on|off|querier on|off|interval <10-3600>|unknown flood|router|router <n> on|off]\n");
		return true;
	}
	mc_apply();
	return true;
}
