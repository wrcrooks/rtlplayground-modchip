/*
 * What the mod chip adds to RTLPlayground's command line and what its SNMP
 * agent reads from the switch. Compiled with sdcc_shim.h, like the modules.
 *
 *   https                    status and the certificate's SHA-256 fingerprint
 *   https on|off             TLS on port 443 (HTTP on port 80 stays)
 *   https newcert            a new key and self-signed certificate
 *   snmp                     status
 *   snmp on|off              SNMPv2c agent on UDP port 161, read-only
 *   snmp community <name>
 *   snmp location <text>
 *   snmp contact <text>
 *   t3                       status of register access over T3 (I2C)
 *   t3 on [100|400|1000]     register access over T3, at that many kHz
 *   t3 off                   register access over the bridge
 *   t3 test                  check T3 reads and writes against the bridge
 *
 * They reach the parser through its CMD_PARSER_EXT hook, so like its own
 * commands they land in the command history and the saved configuration.
 */

#include <stdio.h>
#include <string.h>

#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl837x_port.h"
#include "rtl837x_pins.h"
#include "machine.h"
#include "phy.h"
#include "cmd_parser.h"

#include "plat.h"
#include "dot1x.h"

extern __code const struct machine machine;
extern __xdata uint8_t sfr_data[4];
extern __xdata char port_names[9][PORT_NAME_SIZE];
extern __xdata uint8_t cmd_words_len;
extern __xdata uint8_t cmd_words_b[];
uint8_t cmd_compare(uint8_t start, __code const uint8_t *cmd);

static void ext_error(__code const char *msg)
{
	err_status = ERR_INVALID_ARGUMENT;
	print_string("Error: ");
	print_string(msg);
}


/* The rest of the line from word w, as parse_passwd() takes a password */
static const char *rest(uint8_t w)
{
	return (const char *)&cmd_buffer[cmd_words_b[w]];
}


static bool parse_https(void)
{
	char fp[96];

	if (cmd_words_len == 1) {
		plat_https_status(fp, sizeof(fp));
		print_string("HTTPS ");
		print_string(plat_https_enabled() ? "on" : "off");
		print_string(", certificate SHA-256 ");
		print_string_x(fp);
		write_char('\n');
	} else if (cmd_words_len == 2 && cmd_compare(1, "on")) {
		if (!plat_https_enable(true))
			ext_error("HTTPS could not start\n");
	} else if (cmd_words_len == 2 && cmd_compare(1, "off")) {
		plat_https_enable(false);
	} else if (cmd_words_len == 2 && cmd_compare(1, "newcert")) {
		if (!plat_https_newcert())
			ext_error("could not create a certificate\n");
	} else {
		ext_error("https [on|off|newcert]\n");
	}
	return true;
}


static bool parse_snmp(void)
{
	if (cmd_words_len == 1) {
		print_string("SNMP ");
		print_string(plat_snmp_get(PLAT_SNMP_ENABLED) ? "on" : "off");
		print_string(", community ");
		print_string_x((char *)plat_snmp_get(PLAT_SNMP_COMMUNITY));
		print_string(", location ");
		print_string_x((char *)plat_snmp_get(PLAT_SNMP_LOCATION));
		print_string(", contact ");
		print_string_x((char *)plat_snmp_get(PLAT_SNMP_CONTACT));
		write_char('\n');
	} else if (cmd_words_len == 2 && cmd_compare(1, "on")) {
		plat_snmp_set(PLAT_SNMP_ENABLED, "1");
	} else if (cmd_words_len == 2 && cmd_compare(1, "off")) {
		plat_snmp_set(PLAT_SNMP_ENABLED, NULL);
	} else if (cmd_words_len == 3 && cmd_compare(1, "community")) {
		if (!plat_snmp_set(PLAT_SNMP_COMMUNITY, rest(2)))
			ext_error("community too long\n");
	} else if (cmd_words_len >= 3 && cmd_compare(1, "location")) {
		if (!plat_snmp_set(PLAT_SNMP_LOCATION, rest(2)))
			ext_error("location too long\n");
	} else if (cmd_words_len >= 3 && cmd_compare(1, "contact")) {
		if (!plat_snmp_set(PLAT_SNMP_CONTACT, rest(2)))
			ext_error("contact too long\n");
	} else {
		ext_error("snmp [on|off|community <name>|location <text>|contact <text>]\n");
	}
	return true;
}


bool rtl_bridge_reg_read(uint16_t reg, uint32_t *v);
bool rtl_bridge_reg_write(uint16_t reg, uint32_t v);

/* RTL837x_TBL_DATA_IN_C: a table engine input, which acts only when the
 * host writes TBL_CTRL, so a test can write it. (The seconds counter at
 * 0x06f4 reads back 0 after any write on the RTL8372.) */
#define T3_TEST_REG		0x5cc0

static void t3_value(__code const char *what, uint32_t v)
{
	print_string(what);
	print_long(v);
	write_char('\n');
}


static void t3_line(bool ok, __code const char *what)
{
	print_string(ok ? "  PASS  " : "  FAIL  ");
	print_string(what);
	write_char('\n');
}


/*
 * Bring-up step 2: the chip ID read over T3 and over the bridge must match,
 * and a value written on one path must read back on the other. Register
 * access stays where it is; turn T3 on afterwards if all passes.
 */
static void t3_test(void)
{
	uint32_t a, b, orig;
	bool ok = true;

	if (!plat_t3_raw_read(RTL837X_REG_CHIP_ID, &a)) {
		ext_error("no answer on T3: check SDA/SCL, the pull-ups to the switch's 3.3 V, and that the IF_SEL straps select I2C\n");
		return;
	}
	print_string("T3 chip ID ");
	print_long(a);
	write_char('\n');
	ok &= rtl_bridge_reg_read(RTL837X_REG_CHIP_ID, &b) && a == b;
	t3_line(ok, "chip ID matches the bridge's");
	if (!ok || !rtl_bridge_reg_read(T3_TEST_REG, &orig)) {
		ext_error("T3 reads don't match the bridge\n");
		return;
	}
	b = 0;
	bool w1 = plat_t3_raw_write(T3_TEST_REG, 0x5a5a1234) && rtl_bridge_reg_read(T3_TEST_REG, &b) && b == 0x5a5a1234;
	t3_line(w1, "written over T3, read back over the bridge");
	if (!w1)
		t3_value("  wrote 0x5a5a1234, read ", b);
	a = 0;
	bool w2 = rtl_bridge_reg_write(T3_TEST_REG, 0xa5a5edcb) && plat_t3_raw_read(T3_TEST_REG, &a) && a == 0xa5a5edcb;
	t3_line(w2, "written over the bridge, read back over T3");
	if (!w2)
		t3_value("  wrote 0xa5a5edcb, read ", a);
	rtl_bridge_reg_write(T3_TEST_REG, orig);
	if (!w1 || !w2) {
		ext_error("T3 writes don't work this way: keep register access on the bridge\n");
		return;
	}
	print_string("T3 works; \"t3 on 400\" moves register access over\n");
}


static bool parse_t3(void)
{
	struct plat_t3 st;

	if (cmd_words_len == 1) {
		plat_t3_status(&st);
		print_string("T3 ");
		print_string(!st.up ? "down (switch unpowered)" : st.enabled ? "on" : "off");
		print_string(", ");
		itoa_short(st.khz);
		print_string(" kHz, reads ");
		print_long(st.reads);
		print_string(", writes ");
		print_long(st.writes);
		print_string(", errors ");
		print_long(st.errors);
		write_char('\n');
	} else if (cmd_compare(1, "on") && cmd_words_len <= 3) {
		uint16_t khz = 0;
		if (cmd_words_len == 3) {
			if (cmd_compare(2, "100"))
				khz = 100;
			else if (cmd_compare(2, "400"))
				khz = 400;
			else if (cmd_compare(2, "1000"))
				khz = 1000;
			else {
				ext_error("t3 on [100|400|1000]\n");
				return true;
			}
		}
		plat_t3_enable(true, khz);
	} else if (cmd_words_len == 2 && cmd_compare(1, "off")) {
		plat_t3_enable(false, 0);
	} else if (cmd_words_len == 2 && cmd_compare(1, "test")) {
		t3_test();
	} else {
		ext_error("t3 [on [100|400|1000]|off|test]\n");
	}
	return true;
}


/* CMD_PARSER_EXT, see CMakeLists.txt: true if the command was ours */
/*
 * 802.1X (dot1x.c):
 *   dot1x                                    status
 *   dot1x on|off
 *   dot1x radius <a.b.c.d> [<udp port>]      the RADIUS server, port 1812 by default
 *   dot1x secret <text>                      its shared secret
 *   dot1x reauth <60-86400>|off              periodic re-authentication
 *   dot1x port <n> auto|force-auth|force-unauth
 *   dot1x port <n> mab on|off                MAC authentication for devices without a supplicant
 *   dot1x port <n> restart                   authenticate again now
 */
uint8_t cmd_parse_port_separator(uint8_t idx);
extern __xdata uint8_t atoi_results_u8;

static void dot1x_print_status(void)
{
	char b[160];

	snprintf(b, sizeof(b), "802.1X %s, RADIUS %u.%u.%u.%u:%u, secret %s, re-authentication %s",
		 dot1x_cfg.enabled ? "on" : "off", dot1x_cfg.radius_ip[0], dot1x_cfg.radius_ip[1],
		 dot1x_cfg.radius_ip[2], dot1x_cfg.radius_ip[3], dot1x_cfg.radius_port,
		 dot1x_cfg.secret[0] ? "set" : "not set", dot1x_cfg.reauth_s ? "" : "off");
	print_string_x(b);
	if (dot1x_cfg.reauth_s) {
		snprintf(b, sizeof(b), "every %lu s", (unsigned long)dot1x_cfg.reauth_s);
		print_string_x(b);
	}
	write_char('\n');
	for (uint8_t i = machine.min_port; i <= machine.max_port && i < DOT1X_PORTS; i++) {
		const struct dot1x_status *s = dot1x_port_status(i);
		static const char *const modes[] = { "force-auth", "auto", "force-unauth" };
		if (dot1x_cfg.mode[i] == DOT1X_FORCE_AUTH)
			continue;
		snprintf(b, sizeof(b), "  port %u: %s%s, %s, %s", machine.log_to_phys_port[i], modes[dot1x_cfg.mode[i] % 3],
			 dot1x_cfg.mab[i] ? " + MAB" : "", dot1x_state_name(s->state), s->authorized ? "open" : "closed");
		print_string_x(b);
		if (s->identity[0]) {
			snprintf(b, sizeof(b), ", %s %s", s->method == DOT1X_M_MAB ? "MAC" : "user", s->identity);
			print_string_x(b);
		}
		write_char('\n');
	}
}


/* A decimal number at *s, up to max, then sep (or the end of the word); moves *s past it */
static bool num(const char **s, unsigned max, char sep, unsigned *v)
{
	const char *p = *s;

	*v = 0;
	if (*p < '0' || *p > '9')
		return false;
	while (*p >= '0' && *p <= '9') {
		*v = *v * 10 + (*p++ - '0');
		if (*v > max)
			return false;
	}
	if (sep ? *p != sep : (*p && *p != ' '))
		return false;
	*s = sep ? p + 1 : p;
	return true;
}


static bool parse_ip(const char *s, unsigned a[4])
{
	return num(&s, 255, '.', &a[0]) && num(&s, 255, '.', &a[1]) && num(&s, 255, '.', &a[2]) && num(&s, 255, 0, &a[3]);
}


static bool parse_dot1x(void)
{
	unsigned a[4], port;
	const char *w;

	if (cmd_words_len == 1) {
		dot1x_print_status();
		return true;
	}
	if (cmd_words_len == 2 && (cmd_compare(1, "on") || cmd_compare(1, "off"))) {
		dot1x_cfg.enabled = cmd_compare(1, "on");
	} else if ((cmd_words_len == 3 || cmd_words_len == 4) && cmd_compare(1, "radius")
		   && parse_ip(rest(2), a)) {
		port = 1812;
		w = rest(3);
		if (cmd_words_len == 4 && (!num(&w, 65535, 0, &port) || !port)) {
			ext_error("dot1x radius <a.b.c.d> [<udp port>]\n");
			return true;
		}
		for (int k = 0; k < 4; k++)
			dot1x_cfg.radius_ip[k] = a[k];
		dot1x_cfg.radius_port = port;
	} else if (cmd_words_len >= 3 && cmd_compare(1, "secret")) {
		if (strlen(rest(2)) >= sizeof(dot1x_cfg.secret)) {
			ext_error("the secret is too long\n");
			return true;
		}
		strcpy(dot1x_cfg.secret, rest(2));
	} else if (cmd_words_len == 3 && cmd_compare(1, "reauth")) {
		unsigned s = 0;
		w = rest(2);
		if (!cmd_compare(2, "off") && (!num(&w, 86400, 0, &s) || s < 60)) {
			ext_error("dot1x reauth <60-86400>|off\n");
			return true;
		}
		dot1x_cfg.reauth_s = s;
	} else if (cmd_words_len >= 4 && cmd_compare(1, "port") && cmd_parse_port_separator(cmd_words_b[2])
		   && atoi_results_u8 < DOT1X_PORTS) {
		uint8_t p = atoi_results_u8;
		if (cmd_words_len == 4 && cmd_compare(3, "auto"))
			dot1x_cfg.mode[p] = DOT1X_AUTO;
		else if (cmd_words_len == 4 && cmd_compare(3, "force-auth"))
			dot1x_cfg.mode[p] = DOT1X_FORCE_AUTH;
		else if (cmd_words_len == 4 && cmd_compare(3, "force-unauth"))
			dot1x_cfg.mode[p] = DOT1X_FORCE_UNAUTH;
		else if (cmd_words_len == 5 && cmd_compare(3, "mab") && (cmd_compare(4, "on") || cmd_compare(4, "off")))
			dot1x_cfg.mab[p] = cmd_compare(4, "on");
		else if (cmd_words_len == 4 && cmd_compare(3, "restart")) {
			dot1x_restart(p);
			return true;
		} else {
			ext_error("dot1x port <n> auto|force-auth|force-unauth|restart, dot1x port <n> mab on|off\n");
			return true;
		}
	} else {
		ext_error("dot1x [on|off|radius <a.b.c.d> [<port>]|secret <text>|reauth <s>|off|port <n> ...]\n");
		return true;
	}
	dot1x_apply();
	return true;
}


/*
 * Raw ACL table access, for working out the entry layout on hardware (see
 * docs/acl-reverse-engineering.md). Not configuration: nothing is saved.
 *   acl rule <0-95>                          the rule's data (entry i) and care (entry 0x80 + i) words
 *   acl rule <0-95> <d0>..<d4> <c0>..<c4>    write both halves, hex words, care first
 *   acl action <0-95> [<w0>..<w4>]           the action entry, read or written
 * Table type 1 is the rules, 2 the actions; five 32-bit words each, through
 * TBL_CTRL and DATA_IN (0x5cb8) / DATA_OUT (0x5ccc), as the stock firmware's
 * dal_rtl8373_acl.c uses them.
 */
#define ACL_ENTRIES	96
#define ACL_WORDS	5
#define TBL_ACL_RULE	1
#define TBL_ACL_ACTION	2

static bool acl_op(uint16_t entry, uint8_t type, bool write, uint32_t *w)
{
	uint32_t v;

	if (write)
		for (uint8_t i = 0; i < ACL_WORDS; i++)
			REG_SET(RTL837x_TBL_DATA_IN_A + 4 * i, w[i]);
	REG_SET(RTL837X_TBL_CTRL, (uint32_t)entry << 16 | type << 8 | (write ? TBL_WRITE : 0) | TBL_EXECUTE);
	for (uint8_t tries = 0; ; tries++) {
		reg_read_m(RTL837X_TBL_CTRL);
		v = (uint32_t)sfr_data[0] << 24 | (uint32_t)sfr_data[1] << 16 | sfr_data[2] << 8 | sfr_data[3];
		if (!(v & TBL_EXECUTE))
			break;
		if (tries > 100)
			return false;
	}
	if (!write)
		for (uint8_t i = 0; i < ACL_WORDS; i++) {
			reg_read_m(RTL837x_L2_DATA_OUT_A + 4 * i);
			w[i] = (uint32_t)sfr_data[0] << 24 | (uint32_t)sfr_data[1] << 16 | sfr_data[2] << 8 | sfr_data[3];
		}
	return true;
}


static void acl_print(const char *what, const uint32_t *w)
{
	char b[80];

	snprintf(b, sizeof(b), "%s %08lx %08lx %08lx %08lx %08lx\n", what, (unsigned long)w[0], (unsigned long)w[1],
		 (unsigned long)w[2], (unsigned long)w[3], (unsigned long)w[4]);
	print_string_x(b);
}


static bool hex32(uint8_t word, uint32_t *v)
{
	const char *p = rest(word);

	*v = 0;
	for (uint8_t n = 0; ; n++, p++) {
		char c = *p;
		if (!c || c == ' ')
			return n > 0;
		if (n == 8)
			return false;
		if (c >= '0' && c <= '9')
			*v = *v << 4 | (c - '0');
		else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f')
			*v = *v << 4 | ((c | 0x20) - 'a' + 10);
		else
			return false;
	}
}


static bool parse_acl(void)
{
	uint32_t d[ACL_WORDS], c[ACL_WORDS];
	unsigned idx;
	const char *w = rest(2);
	bool rule = cmd_compare(1, "rule");

	if (cmd_words_len < 3 || !(rule || cmd_compare(1, "action")) || !num(&w, ACL_ENTRIES - 1, 0, &idx)) {
		ext_error("acl rule|action <0-95> [<hex words>]\n");
		return true;
	}
	if (cmd_words_len == 3) {
		if (!acl_op(idx, rule ? TBL_ACL_RULE : TBL_ACL_ACTION, false, d)
		    || (rule && !acl_op(idx | 0x80, TBL_ACL_RULE, false, c))) {
			ext_error("the table engine doesn't answer\n");
			return true;
		}
		acl_print(rule ? "data" : "action", d);
		if (rule)
			acl_print("care", c);
		return true;
	}
	if (cmd_words_len != 3 + (rule ? 2 : 1) * ACL_WORDS) {
		ext_error(rule ? "acl rule <i> <5 data words> <5 care words>\n" : "acl action <i> <5 words>\n");
		return true;
	}
	for (uint8_t i = 0; i < ACL_WORDS; i++)
		if (!hex32(3 + i, &d[i]) || (rule && !hex32(3 + ACL_WORDS + i, &c[i]))) {
			ext_error("words are hex, up to 8 digits\n");
			return true;
		}
	if ((rule && !acl_op(idx | 0x80, TBL_ACL_RULE, true, c))
	    || !acl_op(idx, rule ? TBL_ACL_RULE : TBL_ACL_ACTION, true, d))
		ext_error("the table engine doesn't answer\n");
	return true;
}


bool rtl_cmd_ext(void)
{
	if (cmd_compare(0, "acl"))
		return parse_acl();
	if (cmd_compare(0, "dot1x"))
		return parse_dot1x();
	if (cmd_compare(0, "https"))
		return parse_https();
	if (cmd_compare(0, "snmp"))
		return parse_snmp();
	if (cmd_compare(0, "t3"))
		return parse_t3();
	if (cmd_compare(0, "watchdog") && cmd_words_len == 2 && cmd_compare(1, "test")) {
		plat_watchdog_test();
		return true;
	}
	if (cmd_compare(0, "bridge") && cmd_words_len == 1) {
		plat_bridge_print();
		return true;
	}
	if (cmd_compare(0, "bridge") && cmd_words_len == 2 && cmd_compare(1, "log")) {
		plat_bridge_log();
		return true;
	}
	if (cmd_compare(0, "bridge") && cmd_words_len == 3 && cmd_compare(1, "hold")) {
		plat_bridge_hold(cmd_compare(2, "on"));
		return true;
	}
	if (cmd_compare(0, "bridge") && cmd_words_len == 2 && cmd_compare(1, "raw")) {
		plat_bridge_raw();
		return true;
	}
	if (cmd_compare(0, "bridge") && cmd_words_len == 2 && cmd_compare(1, "probe")) {
		plat_bridge_probe();
		return true;
	}
	return false;
}

/* ---- the switch ports, for the SNMP agent's interface tables ---- */

uint8_t rtl_port_count(void)
{
	return machine.max_port - machine.min_port + 1;
}


/* One port; the SNMP agent and the dashboard read them all with rtl_ports_read() */
bool rtl_port_read(uint8_t phys, struct rtl_port *p)
{
	struct rtl_port all[RTL_MAX_PORTS];

	if (phys < 1 || phys > rtl_ports_read(all, RTL_MAX_PORTS))
		return false;
	*p = all[phys - 1];
	return true;
}
