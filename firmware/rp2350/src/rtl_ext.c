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

#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl837x_port.h"
#include "rtl837x_pins.h"
#include "machine.h"
#include "phy.h"
#include "cmd_parser.h"

#include "plat.h"

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
bool rtl_cmd_ext(void)
{
	if (cmd_compare(0, "https"))
		return parse_https();
	if (cmd_compare(0, "snmp"))
		return parse_snmp();
	if (cmd_compare(0, "t3"))
		return parse_t3();
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
