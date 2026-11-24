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

#include <stdarg.h>
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
#include "rtl837x_acl.h"
#include "rtl_ext.h"
#include "safety.h"

extern __code const struct machine machine;
extern __xdata uint8_t sfr_data[4];
extern __xdata char port_names[9][PORT_NAME_SIZE];

void ext_error(__code const char *msg)
{
	err_status = ERR_INVALID_ARGUMENT;
	print_string("Error: ");
	print_string(msg);
}


/* The rest of the line from word w, as parse_passwd() takes a password */
const char *ext_rest(uint8_t w)
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
	} else if (cmd_words_len == 2 && cmd_compare(1, "cert")) {
		char pem[1600];

		if (plat_https_cert(pem, sizeof(pem)))
			print_string_x(pem);
		else
			ext_error("there is no certificate\n");
	} else if (cmd_words_len == 2 && cmd_compare(1, "newcert")) {
		if (!plat_https_newcert())
			ext_error("could not create a certificate\n");
	} else {
		ext_error("https [on|off|newcert|cert]\n");
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
		char st[600];
		plat_snmp_status(st, sizeof(st));
		print_string_x(st);
	} else if (cmd_words_len == 2 && cmd_compare(1, "on")) {
		plat_snmp_set(PLAT_SNMP_ENABLED, "1");
	} else if (cmd_words_len == 2 && cmd_compare(1, "off")) {
		plat_snmp_set(PLAT_SNMP_ENABLED, NULL);
	} else if (cmd_words_len == 3 && cmd_compare(1, "community")) {
		if (!plat_snmp_set(PLAT_SNMP_COMMUNITY, ext_rest(2)))
			ext_error("community too long\n");
	} else if (cmd_words_len >= 3 && cmd_compare(1, "location")) {
		if (!plat_snmp_set(PLAT_SNMP_LOCATION, ext_rest(2)))
			ext_error("location too long\n");
	} else if (cmd_words_len >= 3 && cmd_compare(1, "contact")) {
		if (!plat_snmp_set(PLAT_SNMP_CONTACT, ext_rest(2)))
			ext_error("contact too long\n");
	} else if (cmd_words_len == 3 && cmd_compare(1, "v2c") && (cmd_compare(2, "on") || cmd_compare(2, "off"))) {
		plat_snmp_v2c(cmd_compare(2, "on"));
	} else if (cmd_words_len == 4 && cmd_compare(1, "user") && cmd_compare(3, "off")) {
		char name[33];

		if (!ext_word(2, name, sizeof(name)) || !plat_snmp_user(name, false, NULL, NULL))
			ext_error("no such user\n");
	} else if ((cmd_words_len == 6 || (cmd_words_len == 8 && cmd_compare(6, "aes"))) && cmd_compare(1, "user")
		   && cmd_compare(3, "auth") && (cmd_compare(4, "sha") || cmd_compare(4, "sha256"))) {
		char name[33], auth[65], priv[65];

		if (!ext_word(2, name, sizeof(name)) || !ext_word(5, auth, sizeof(auth))
		    || (cmd_words_len == 8 && !ext_word(7, priv, sizeof(priv)))
		    || !plat_snmp_user(name, cmd_compare(4, "sha256"), auth, cmd_words_len == 8 ? priv : NULL))
			ext_error("passwords have 8 to 64 characters, and there can be 4 users\n");
	} else if ((cmd_words_len == 3 || cmd_words_len == 4) && cmd_compare(1, "trap")) {
		uint8_t ip[4];
		unsigned a[4];
		char user[33];

		if (!ext_ip(ext_rest(2), a) || (cmd_words_len == 4 && !ext_word(3, user, sizeof(user)))) {
			ext_error("snmp trap <a.b.c.d> [<SNMPv3 user>|off]\n");
			return true;
		}
		for (int k = 0; k < 4; k++)
			ip[k] = a[k];
		if (!plat_snmp_trap(ip, cmd_words_len == 4 ? user : NULL, cmd_words_len == 3 || !cmd_compare(3, "off")))
			ext_error("traps go to at most two addresses\n");
	} else {
		ext_error("snmp [on|off|community <name>|location <text>|contact <text>|v2c on|off|\n"
			  "  user <name> auth sha|sha256 <password> [aes <password>]|user <name> off|\n"
			  "  trap <a.b.c.d> [<user>|off]]\n");
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
 *   dot1x radsec <fingerprint>|off           over TLS, to the server with that certificate (SHA-256);
 *                                            the port is then 2083 by default
 *   dot1x reauth <60-86400>|off              periodic re-authentication
 *   dot1x port <n> auto|force-auth|force-unauth
 *   dot1x port <n> mab on|off                MAC authentication for devices without a supplicant
 *   dot1x port <n> restart                   authenticate again now
 */

static void dot1x_print_status(void)
{
	char b[160];

	snprintf(b, sizeof(b), "802.1X %s, RADIUS %u.%u.%u.%u:%u%s, secret %s, re-authentication %s",
		 dot1x_cfg.enabled ? "on" : "off", dot1x_cfg.radius_ip[0], dot1x_cfg.radius_ip[1],
		 dot1x_cfg.radius_ip[2], dot1x_cfg.radius_ip[3], dot1x_cfg.radius_port, dot1x_cfg.radsec ? " over TLS" : "",
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
bool ext_num(const char **s, unsigned max, char sep, unsigned *v)
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


bool ext_ip(const char *s, unsigned a[4])
{
	return ext_num(&s, 255, '.', &a[0]) && ext_num(&s, 255, '.', &a[1]) && ext_num(&s, 255, '.', &a[2]) && ext_num(&s, 255, 0, &a[3]);
}


const char *ext_word(uint8_t w, char *buf, uint8_t max)
{
	const char *p = ext_rest(w);
	uint8_t n = 0;

	while (p[n] && p[n] != ' ') {
		if (n + 1 >= max)
			return NULL;
		buf[n] = p[n];
		n++;
	}
	buf[n] = 0;
	return buf;
}


bool ext_word_num(uint8_t w, unsigned min, unsigned max, unsigned *v)
{
	const char *p = ext_rest(w);

	return ext_num(&p, max, 0, v) && *v >= min;
}


/* The line being typed or parsed is kept: commands share one buffer */
extern __xdata char save_cmd;

bool ext_run(const char *fmt, ...)
{
	static uint8_t line[CMD_BUF_SIZE];
	char cmd[CMD_BUF_SIZE];
	uint8_t words = cmd_words_len, err = err_status;
	char save = save_cmd;
	va_list ap;
	bool ok;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	memcpy(line, cmd_buffer, sizeof(line));
	save_cmd = 0;
	execute_commands((__xdata uint8_t *)cmd);
	ok = err_status == ERR_OK;
	save_cmd = save;
	err_status = err;
	cmd_words_len = words;
	memcpy(cmd_buffer, line, sizeof(line));
	return ok;
}


/*
 *   mqtt                                status
 *   mqtt on|off
 *   mqtt server <a.b.c.d> [<port>]      1883, or 8883 with TLS
 *   mqtt login <user> <password>|off
 *   mqtt topic <prefix>|off             off: the hostname
 *   mqtt tls <fingerprint>|off          TLS, to the server with that certificate (SHA-256)
 *   mqtt interval <5-3600>
 */
static bool parse_mqtt(void)
{
	char a[70], b[70];
	unsigned ip[4], v = 0;
	bool ok = true;

	if (cmd_words_len == 1) {
		char st[200];

		plat_mqtt_status(st, sizeof(st));
		print_string_x(st);
	} else if (cmd_words_len == 2 && (cmd_compare(1, "on") || cmd_compare(1, "off"))) {
		plat_mqtt_enable(cmd_compare(1, "on"));
	} else if ((cmd_words_len == 3 || cmd_words_len == 4) && cmd_compare(1, "server") && ext_ip(ext_rest(2), ip)) {
		uint8_t i[4] = { ip[0], ip[1], ip[2], ip[3] };

		ok = cmd_words_len == 3 || ext_word_num(3, 1, 65535, &v);
		if (ok)
			plat_mqtt_server(i, v);
	} else if (cmd_words_len == 3 && cmd_compare(1, "login") && cmd_compare(2, "off")) {
		plat_mqtt_login("", "");
	} else if (cmd_words_len == 4 && cmd_compare(1, "login")) {
		ok = ext_word(2, a, sizeof(a)) && ext_word(3, b, sizeof(b)) && plat_mqtt_login(a, b);
	} else if (cmd_words_len == 3 && cmd_compare(1, "topic")) {
		ok = plat_mqtt_topic(cmd_compare(2, "off") ? "" : ext_rest(2));
	} else if (cmd_words_len == 3 && cmd_compare(1, "tls")) {
		ok = plat_mqtt_tls(cmd_compare(2, "off") ? NULL : ext_rest(2));
	} else if (cmd_words_len == 3 && cmd_compare(1, "interval") && ext_word_num(2, 5, 3600, &v)) {
		plat_mqtt_interval(v);
	} else {
		ok = false;
	}
	if (!ok)
		ext_error("mqtt [on|off|server <a.b.c.d> [<port>]|login <user> <password>|off|topic <prefix>|off|\n"
			  "  tls <SHA-256 fingerprint>|off|interval <5-3600>]\n");
	return true;
}


/*
 *   ssh                                 status, the host key's fingerprint
 *   ssh on|off|newkey
 */
static bool parse_ssh(void)
{
	char st[120];
	bool ok = true;

	if (cmd_words_len == 1) {
		plat_ssh_status(st, sizeof(st));
		print_string_x(st);
	} else if (cmd_words_len == 2 && (cmd_compare(1, "on") || cmd_compare(1, "off"))) {
		ok = plat_ssh_enable(cmd_compare(1, "on"));
	} else if (cmd_words_len == 2 && cmd_compare(1, "newkey")) {
		ok = plat_ssh_newkey();
	} else {
		ext_error("ssh [on|off|newkey]\n");
	}
	if (!ok)
		ext_error("no host key could be made\n");
	return true;
}


/*
 *   prometheus on|off                   GET /metrics without a login
 *   prometheus token <text>|off         but with "Authorization: Bearer <text>"
 */
static bool parse_prometheus(void)
{
	char st[80];

	if (cmd_words_len == 1) {
		plat_prometheus_status(st, sizeof(st));
		print_string_x(st);
	} else if (cmd_words_len == 2 && (cmd_compare(1, "on") || cmd_compare(1, "off"))) {
		plat_prometheus(cmd_compare(1, "on"));
	} else if (!(cmd_words_len == 3 && cmd_compare(1, "token")
		     && plat_prometheus_token(cmd_compare(2, "off") ? "" : ext_rest(2)))) {
		ext_error("prometheus [on|off|token <text>|off]\n");
	}
	return true;
}


/*
 *   ip6                        status
 *   ip6 on|off                 IPv6 for management: link-local and SLAAC addresses
 *   ip6 addr <address>|none    and one of our own (/64)
 *   ip6 gw <address>|auto      the gateway, or as router advertisements say
 */
static bool parse_ip6(void)
{
	char st[400];
	bool ok = true;

	if (cmd_words_len == 1) {
		plat_ip6_status(st, sizeof(st));
		print_string_x(st);
	} else if (cmd_words_len == 2 && (cmd_compare(1, "on") || cmd_compare(1, "off"))) {
		plat_ip6_enable(cmd_compare(1, "on"));
	} else if (cmd_words_len == 3 && cmd_compare(1, "addr")) {
		ok = plat_ip6_addr(cmd_compare(2, "none") ? NULL : ext_rest(2));
	} else if (cmd_words_len == 3 && cmd_compare(1, "gw")) {
		ok = plat_ip6_gw(cmd_compare(2, "auto") ? NULL : ext_rest(2));
	} else {
		ok = false;
	}
	if (!ok)
		ext_error("ip6 [on|off|addr <address>|none|gw <address>|auto]\n");
	return true;
}


/*
 *   reboot                     what is pending
 *   reboot now                 the switch and the mod chip
 *   reboot in <1-1440>         in that many minutes
 *   reboot cancel
 *   rollback                   what is pending
 *   rollback in <1-1440>       back to the configuration saved now, unless confirmed
 *   rollback confirm
 * Not configuration: nothing of it is saved.
 */
static bool parse_reboot(void)
{
	bool rb = cmd_compare(0, "rollback");
	unsigned v;
	char b[80];

	if (cmd_words_len == 1) {
		uint32_t left = rb ? safety_rollback_left() : safety_reboot_left();

		if (left)
			snprintf(b, sizeof(b), "%s in %lu s\n", rb ? "Rollback" : "Restart", (unsigned long)left);
		else
			snprintf(b, sizeof(b), "No %s pending\n", rb ? "rollback" : "restart");
		print_string_x(b);
	} else if (cmd_words_len == 3 && cmd_compare(1, "in") && ext_word_num(2, 1, SAFETY_MAX_MIN, &v)) {
		if (!rb)
			safety_reboot_in(v);
		else if (!safety_rollback_start(v))
			ext_error("the saved configuration could not be kept\n");
	} else if (!rb && cmd_words_len == 2 && cmd_compare(1, "now")) {
		plat_reboot();
	} else if (!rb && cmd_words_len == 2 && cmd_compare(1, "cancel")) {
		safety_reboot_in(0);
	} else if (rb && cmd_words_len == 2 && cmd_compare(1, "confirm")) {
		safety_rollback_confirm();
	} else {
		ext_error(rb ? "rollback [in <1-1440>|confirm]\n" : "reboot [now|in <1-1440>|cancel]\n");
	}
	return true;
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
		   && ext_ip(ext_rest(2), a)) {
		port = dot1x_cfg.radsec ? 2083 : 1812;
		w = ext_rest(3);
		if (cmd_words_len == 4 && (!ext_num(&w, 65535, 0, &port) || !port)) {
			ext_error("dot1x radius <a.b.c.d> [<udp port>]\n");
			return true;
		}
		for (int k = 0; k < 4; k++)
			dot1x_cfg.radius_ip[k] = a[k];
		dot1x_cfg.radius_port = port;
		plat_radsec_restart();
	} else if (cmd_words_len >= 3 && cmd_compare(1, "secret")) {
		if (strlen(ext_rest(2)) >= sizeof(dot1x_cfg.secret)) {
			ext_error("the secret is too long\n");
			return true;
		}
		strcpy(dot1x_cfg.secret, ext_rest(2));
	} else if (cmd_words_len == 3 && cmd_compare(1, "radsec")) {
		bool off = cmd_compare(2, "off");

		if (!plat_radsec(off ? NULL : ext_rest(2))) {
			ext_error("dot1x radsec <SHA-256 fingerprint>|off\n");
			return true;
		}
		/* the other way's usual port follows along */
		if (!off && (dot1x_cfg.radius_port == 1812 || !dot1x_cfg.radius_port))
			dot1x_cfg.radius_port = 2083;
		else if (off && dot1x_cfg.radius_port == 2083)
			dot1x_cfg.radius_port = 1812;
		dot1x_cfg.radsec = !off;
	} else if (cmd_words_len == 3 && cmd_compare(1, "reauth")) {
		unsigned s = 0;
		w = ext_rest(2);
		if (!cmd_compare(2, "off") && (!ext_num(&w, 86400, 0, &s) || s < 60)) {
			ext_error("dot1x reauth <60-86400>|off\n");
			return true;
		}
		dot1x_cfg.reauth_s = s;
	} else if (cmd_words_len == 3 && cmd_compare(1, "radius-vlan") && (cmd_compare(2, "on") || cmd_compare(2, "off"))) {
		dot1x_cfg.radius_vlan = cmd_compare(2, "on");
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
		else if (cmd_words_len == 5 && (cmd_compare(3, "guest-vlan") || cmd_compare(3, "auth-fail-vlan"))) {
			unsigned v = 0;

			if (!cmd_compare(4, "off") && !ext_word_num(4, 1, 4094, &v)) {
				ext_error("dot1x port <n> guest-vlan|auth-fail-vlan <1-4094>|off\n");
				return true;
			}
			if (cmd_compare(3, "guest-vlan"))
				dot1x_cfg.guest_vlan[p] = v;
			else
				dot1x_cfg.fail_vlan[p] = v;
		} else if (cmd_words_len == 4 && cmd_compare(3, "restart")) {
			dot1x_restart(p);
			return true;
		} else {
			ext_error("dot1x port <n> auto|force-auth|force-unauth|restart, dot1x port <n> mab on|off,\n"
				  "  dot1x port <n> guest-vlan|auth-fail-vlan <1-4094>|off\n");
			return true;
		}
	} else {
		ext_error("dot1x [on|off|radius <a.b.c.d> [<port>]|secret <text>|radsec <fingerprint>|off|reauth <s>|off|radius-vlan on|off|port <n> ...]\n");
		return true;
	}
	dot1x_apply();
	return true;
}


/*
 * Raw ACL table access, for working out the entry layout on hardware (see
 * docs/acl-reverse-engineering.md). Not configuration: nothing is saved.
 *   acltbl rule <0-95>                        the rule's data (entry i) and care (entry 0x80 + i) words
 *   acltbl rule <0-95> <d0>..<d4> <c0>..<c4>  write both halves, hex words, care first
 *   acltbl action <0-95> [<w0>..<w4>]         the action entry, read or written
 * The rules themselves are RTLPlayground's acl command (rtl837x_acl.c).
 * Table type 1 is the rules, 2 the actions; five 32-bit words each, through
 * TBL_CTRL and DATA_IN (0x5cb8) / DATA_OUT (0x5ccc), as the stock firmware's
 * dal_rtl8373_acl.c uses them.
 */
#define ACL_ENTRIES	96
#define ACL_WORDS	5

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
	const char *p = ext_rest(word);

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
	const char *w = ext_rest(2);
	bool rule = cmd_compare(1, "rule");

	if (cmd_words_len < 3 || !(rule || cmd_compare(1, "action")) || !ext_num(&w, ACL_ENTRIES - 1, 0, &idx)) {
		ext_error("acltbl rule|action <0-95> [<hex words>]\n");
		return true;
	}
	if (cmd_words_len == 3) {
		if (!acl_op(idx, rule ? TBL_ACL_RULE : TBL_ACL_ACT, false, d)
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
		ext_error(rule ? "acltbl rule <i> <5 data words> <5 care words>\n" : "acltbl action <i> <5 words>\n");
		return true;
	}
	for (uint8_t i = 0; i < ACL_WORDS; i++)
		if (!hex32(3 + i, &d[i]) || (rule && !hex32(3 + ACL_WORDS + i, &c[i]))) {
			ext_error("words are hex, up to 8 digits\n");
			return true;
		}
	if ((rule && !acl_op(idx | 0x80, TBL_ACL_RULE, true, c))
	    || !acl_op(idx, rule ? TBL_ACL_RULE : TBL_ACL_ACT, true, d))
		ext_error("the table engine doesn't answer\n");
	return true;
}


bool rtl_cmd_ext(void)
{
	if (cmd_compare(0, "reboot") || cmd_compare(0, "rollback"))
		return parse_reboot();
	if (cmd_compare(0, "mqtt"))
		return parse_mqtt();
	if (cmd_compare(0, "ssh"))
		return parse_ssh();
	if (cmd_compare(0, "prometheus"))
		return parse_prometheus();
	if (cmd_compare(0, "ip6"))
		return parse_ip6();
	if (rtl_guard_cmd() || rtl_mcast_cmd())
		return true;
	if (cmd_compare(0, "acltbl"))
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
