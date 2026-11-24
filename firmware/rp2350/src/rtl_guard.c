/*
 * Error-disable, DHCP snooping and QoS, see rtl_guard.h. Compiled with
 * sdcc_shim.h, like the modules.
 *
 *   errdisable                              status
 *   errdisable recovery <30-86400>|off      seconds until a port comes back, 300 at first
 *   errdisable cause bpduguard|l2-limit|link-flap on|off
 *   errdisable port <n> clear
 *
 *   dhcp-snoop                              status
 *   dhcp-snoop on|off                       drop DHCP servers' replies on untrusted ports
 *   dhcp-snoop trust <n> on|off             a port servers may be behind
 *   dhcp-snoop ra on|off                    IPv6 router advertisements too
 *
 *   qos                                     status
 *   qos port <n> trust dscp|none            priority from the IPv4 DSCP's class
 *   qos port <n> priority <0-7>             priority of what else comes in
 *
 * DHCP snooping and QoS are ACL rules, through RTLPlayground's acl command:
 * 41-47 DSCP classes, 48-57 port priorities, 60-62 DHCP and router
 * advertisements (and field selector 15). They go in from rtl_guard_second(),
 * as running a command from within another would overwrite its line.
 */

#include <stdio.h>
#include <string.h>

#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl837x_port.h"
#include "rtl837x_stp.h"
#include "rtl837x_l2sec.h"
#include "machine.h"
#include "cmd_parser.h"

#include "events.h"
#include "plat.h"
#include "rtl_ext.h"
#include "rtl_guard.h"

extern __code const struct machine machine;
extern __xdata uint8_t sfr_data[4];

#define PORTS			10
#define RECOVERY_S		300
#define FLAP_WINDOW_S		10
#define FLAP_CHANGES		6	/* three times down and up */

enum cause { CAUSE_BPDU, CAUSE_LIMIT, CAUSE_FLAP, CAUSES };
static const char *const cause_names[CAUSES] = { "bpduguard", "l2-limit", "link-flap" };

static bool cause_on[CAUSES];
static uint32_t recovery_s;
static uint16_t disabled;
static uint8_t why[PORTS];
static uint32_t left[PORTS];
static uint32_t flap_at[PORTS];
static uint8_t flaps[PORTS];

static bool snoop_on, snoop_ra, snoop_dirty;
static uint16_t snoop_trust;
static bool snoop_in;			/* its rules are in the table */
static bool qos_dirty;
static uint16_t qos_trust;
static uint8_t qos_prio[PORTS];		/* 0xff: not set */
static bool qos_in;

#define RULE_QOS_CLASS	40		/* + 1..7 */
#define RULE_QOS_PORT	48		/* + front port - 1 */
#define RULE_SNOOP4	60
#define RULE_SNOOP6	61
#define RULE_SNOOP_RA	62
#define FIELD_SNOOP_RA	15

#define FOR_PORTS(i)	for (uint8_t i = machine.min_port; i <= machine.max_port && i < PORTS; i++)


/* ---- events on the console, which syslog follows ---- */

static void console_event(const struct event *e)
{
	print_string_x((char *)e->text);
	write_char('\n');
}


/* ---- error-disable ---- */

uint16_t rtl_errdis(void)
{
	return disabled;
}


bool rtl_errdis_limit(uint8_t port)
{
	(void)port;
	return cause_on[CAUSE_LIMIT];
}


static void disable_port(uint8_t port, uint8_t cause)
{
	uint8_t front = machine.log_to_phys_port[port];

	if ((disabled >> port) & 1)
		return;
	disabled |= 1 << port;
	why[port] = cause;
	left[port] = recovery_s;
	rtl_iso_errdis(disabled);
	port_l2_forget_port(port);
	event(EV_ERRDISABLE, front, 1, "Port %u error-disabled: %s", front, cause_names[cause]);
}


static void enable_port(uint8_t port)
{
	uint8_t front = machine.log_to_phys_port[port], ent = port < STP_PORTS ? stp_ent_of[port] : port;

	if (!((disabled >> port) & 1))
		return;
	disabled &= ~(1 << port);
	flaps[port] = 0;
	rtl_iso_errdis(disabled);
	if (why[port] == CAUSE_BPDU && stp_enabled) {
		/* rejoin as "stp port <n> on" does, and listen first */
		if (ent >= STP_LAG_BASE)
			ext_run("stp lag %u on", ent - STP_LAG_BASE + 1);
		else
			ext_run("stp port %u on", front);
	}
	event(EV_ERRDISABLE, front, 0, "Port %u recovered from error-disable", front);
}


void rtl_guard_link(uint8_t port, uint8_t link)
{
	uint32_t now = plat_uptime_s();

	(void)link;
	if (port >= PORTS || !cause_on[CAUSE_FLAP] || ((disabled >> port) & 1))
		return;
	if (!flaps[port] || now - flap_at[port] > FLAP_WINDOW_S) {
		flap_at[port] = now;
		flaps[port] = 0;
	}
	if (++flaps[port] >= FLAP_CHANGES)
		disable_port(port, CAUSE_FLAP);
}


static uint16_t learned(uint8_t port)
{
	reg_read_m(RTL837X_L2_LRN_PORT_CONSTRT_CNT + (port << 2));
	return ((sfr_data[2] << 8) | sfr_data[3]) & L2_LRN_PORT_CNT_MASK;
}


static void errdis_second(void)
{
	FOR_PORTS(i) {
		if ((disabled >> i) & 1) {
			if (left[i] && !--left[i])
				enable_port(i);
			continue;
		}
		if (cause_on[CAUSE_BPDU] && stp_enabled && (stp_pflags[stp_ent_of[i]] & STP_PF_TRIPPED))
			disable_port(i, CAUSE_BPDU);
		/* With this cause on, the switch may learn one address more than
		 * the limit (rtl_env.c), which is how a violation shows */
		if (cause_on[CAUSE_LIMIT] && rtl_l2_limit(i) < L2_LEARN_UNLIMITED && learned(i) > rtl_l2_limit(i))
			disable_port(i, CAUSE_LIMIT);
	}
}


/* ---- DHCP snooping and QoS, as ACL rules ---- */

/* " 1 3 4", the front ports in mask; false if there are none */
static bool port_list(uint16_t mask, char *buf, size_t max)
{
	size_t n = 0;

	buf[0] = 0;
	for (uint8_t f = 1; f <= PORTS; f++)
		FOR_PORTS(i)
			if (machine.log_to_phys_port[i] == f && ((mask >> i) & 1))
				n = app(buf, max, n, " %u", f);
	return n > 0;
}


static uint16_t all_ports(void)
{
	uint16_t m = 0;

	FOR_PORTS(i)
		m |= 1 << i;
	return m;
}


static void snoop_apply(void)
{
	char ports[40];
	bool ok = true;

	snoop_dirty = false;
	if (snoop_in) {
		ext_run("acl %u off", RULE_SNOOP4);
		ext_run("acl %u off", RULE_SNOOP6);
		ext_run("acl %u off", RULE_SNOOP_RA);
		ext_run("acl field %u off", FIELD_SNOOP_RA);
		snoop_in = false;
	}
	if (!snoop_on || !port_list(all_ports() & ~snoop_trust, ports, sizeof(ports)))
		return;
	snoop_in = true;
	ok &= ext_run("acl %u udp sport 67 drop%s", RULE_SNOOP4, ports);
	ok &= ext_run("acl %u udp sport 547 drop%s", RULE_SNOOP6, ports);
	if (snoop_ra) {
		ok &= ext_run("acl field %u l4 0", FIELD_SNOOP_RA);
		ok &= ext_run("acl %u ipv6 icmp field %u 8600/ff00 drop%s", RULE_SNOOP_RA, FIELD_SNOOP_RA, ports);
	}
	if (!ok)
		event(EV_CONFIG, 0, 0, "DHCP snooping: the ACL table took not all of its rules");
}


static void qos_apply(void)
{
	char ports[40];
	bool ok = true;

	qos_dirty = false;
	if (qos_in) {
		for (uint8_t c = 1; c < 8; c++)
			ext_run("acl %u off", RULE_QOS_CLASS + c);
		for (uint8_t f = 1; f <= PORTS; f++)
			ext_run("acl %u off", RULE_QOS_PORT + f - 1);
		qos_in = false;
	}
	if (port_list(qos_trust, ports, sizeof(ports))) {
		qos_in = true;
		for (uint8_t c = 1; c < 8; c++)
			ok &= ext_run("acl %u ipv4 tos %02x/e0 priority %u%s", RULE_QOS_CLASS + c, c << 5, c, ports);
	}
	FOR_PORTS(i) {
		uint8_t f = machine.log_to_phys_port[i];

		if (qos_prio[i] > 7)
			continue;
		qos_in = true;
		ok &= ext_run("acl %u priority %u %u", RULE_QOS_PORT + f - 1, qos_prio[i], f);
	}
	if (!ok)
		event(EV_CONFIG, 0, 0, "QoS: the ACL table took not all of its rules");
}


/* ---- */

void rtl_guard_reset(void)
{
	cause_on[CAUSE_BPDU] = true;
	cause_on[CAUSE_LIMIT] = cause_on[CAUSE_FLAP] = false;
	recovery_s = RECOVERY_S;
	disabled = 0;
	memset(flaps, 0, sizeof(flaps));
	snoop_on = snoop_ra = snoop_dirty = snoop_in = false;
	snoop_trust = 0;
	qos_dirty = qos_in = false;
	qos_trust = 0;
	memset(qos_prio, 0xff, sizeof(qos_prio));
	rtl_iso_errdis(0);
	event_listen(console_event);
}


void rtl_guard_second(void)
{
	errdis_second();
	if (snoop_dirty)
		snoop_apply();
	if (qos_dirty)
		qos_apply();
}


size_t rtl_guard_json(char *buf, size_t max)
{
	size_t n = app(buf, max, 0, "{\"recovery\":%lu,\"causes\":{", (unsigned long)recovery_s);
	bool comma = false;

	for (uint8_t c = 0; c < CAUSES; c++)
		n = app(buf, max, n, "%s\"%s\":%d", c ? "," : "", cause_names[c], cause_on[c]);
	n = app(buf, max, n, "},\"snoop\":%d,\"ra\":%d,\"ports\":[", snoop_on, snoop_ra);
	for (uint8_t f = 1; f <= PORTS; f++)
		FOR_PORTS(i) {
			if (machine.log_to_phys_port[i] != f)
				continue;
			n = app(buf, max, n, "%s{\"port\":%u,\"disabled\":\"%s\",\"left\":%lu,\"trust\":%d,\"dscp\":%d,\"prio\":%d}",
				comma ? "," : "", f, (disabled >> i) & 1 ? cause_names[why[i]] : "",
				(unsigned long)((disabled >> i) & 1 ? left[i] : 0), (snoop_trust >> i) & 1,
				(qos_trust >> i) & 1, qos_prio[i] > 7 ? -1 : qos_prio[i]);
			comma = true;
		}
	return app(buf, max, n, "]}");
}


static bool on_off(uint8_t w, bool *v)
{
	if (cmd_words_len != w + 1 || (!cmd_compare(w, "on") && !cmd_compare(w, "off")))
		return false;
	*v = cmd_compare(w, "on");
	return true;
}


static bool parse_errdisable(void)
{
	char b[80];
	unsigned s;

	if (cmd_words_len == 1) {
		snprintf(b, sizeof(b), "Recovery %lu s%s", (unsigned long)recovery_s, recovery_s ? "" : " (off)");
		print_string_x(b);
		for (uint8_t c = 0; c < CAUSES; c++) {
			snprintf(b, sizeof(b), ", %s %s", cause_names[c], cause_on[c] ? "on" : "off");
			print_string_x(b);
		}
		write_char('\n');
		FOR_PORTS(i)
			if ((disabled >> i) & 1) {
				snprintf(b, sizeof(b), "  port %u: %s, %lu s left\n", machine.log_to_phys_port[i],
					 cause_names[why[i]], (unsigned long)left[i]);
				print_string_x(b);
			}
		return true;
	}
	if (cmd_words_len == 3 && cmd_compare(1, "recovery")) {
		if (cmd_compare(2, "off"))
			recovery_s = 0;
		else if (ext_word_num(2, 30, 86400, &s))
			recovery_s = s;
		else
			goto usage;
		return true;
	}
	if (cmd_compare(1, "cause") && cmd_words_len == 4)
		for (uint8_t c = 0; c < CAUSES; c++)
			if (cmd_compare(2, cause_names[c]) && on_off(3, &cause_on[c])) {
				rtl_iso_errdis(disabled);	/* the learning limits follow */
				return true;
			}
	if (cmd_words_len == 4 && cmd_compare(1, "port") && cmd_parse_port_separator(cmd_words_b[2])
	    && cmd_compare(3, "clear")) {
		/* from the next second: enable_port() may run a command */
		if ((disabled >> atoi_results_u8) & 1)
			left[atoi_results_u8] = 1;
		return true;
	}
usage:
	ext_error("errdisable [recovery <30-86400>|off] [cause bpduguard|l2-limit|link-flap on|off] [port <n> clear]\n");
	return true;
}


static bool parse_snoop(void)
{
	char b[100], list[40];
	bool v;

	if (cmd_words_len == 1) {
		port_list(snoop_trust, list, sizeof(list));
		snprintf(b, sizeof(b), "DHCP snooping %s, router advertisements %s, trusted ports:%s\n", snoop_on ? "on" : "off",
			 snoop_ra ? "guarded" : "not guarded", list[0] ? list : " none");
		print_string_x(b);
		return true;
	}
	if (on_off(1, &snoop_on) || (cmd_compare(1, "ra") && on_off(2, &snoop_ra))) {
		snoop_dirty = true;
		return true;
	}
	if (cmd_compare(1, "trust") && cmd_words_len == 4 && cmd_parse_port_separator(cmd_words_b[2])
	    && on_off(3, &v)) {
		snoop_trust = v ? snoop_trust | 1 << atoi_results_u8 : snoop_trust & ~(1 << atoi_results_u8);
		snoop_dirty = true;
		return true;
	}
	ext_error("dhcp-snoop [on|off|trust <n> on|off|ra on|off]\n");
	return true;
}


static bool parse_qos(void)
{
	char b[100], list[40];
	unsigned v;

	if (cmd_words_len == 1) {
		port_list(qos_trust, list, sizeof(list));
		snprintf(b, sizeof(b), "Ports trusting DSCP:%s\n", list[0] ? list : " none");
		print_string_x(b);
		FOR_PORTS(i)
			if (qos_prio[i] <= 7) {
				snprintf(b, sizeof(b), "  port %u: priority %u\n", machine.log_to_phys_port[i], qos_prio[i]);
				print_string_x(b);
			}
		return true;
	}
	if (cmd_words_len == 5 && cmd_compare(1, "port") && cmd_parse_port_separator(cmd_words_b[2])
	    && atoi_results_u8 < PORTS) {
		uint8_t p = atoi_results_u8;

		if (cmd_compare(3, "trust") && cmd_compare(4, "dscp"))
			qos_trust |= 1 << p;
		else if (cmd_compare(3, "trust") && cmd_compare(4, "none"))
			qos_trust &= ~(1 << p);
		else if (cmd_compare(3, "priority") && cmd_compare(4, "off"))
			qos_prio[p] = 0xff;
		else if (cmd_compare(3, "priority") && ext_word_num(4, 0, 7, &v))
			qos_prio[p] = v;
		else
			goto usage;
		qos_dirty = true;
		return true;
	}
usage:
	ext_error("qos port <n> trust dscp|none, qos port <n> priority <0-7>|off\n");
	return true;
}


bool rtl_guard_cmd(void)
{
	if (cmd_compare(0, "errdisable"))
		return parse_errdisable();
	if (cmd_compare(0, "dhcp-snoop"))
		return parse_snoop();
	if (cmd_compare(0, "qos"))
		return parse_qos();
	return false;
}
