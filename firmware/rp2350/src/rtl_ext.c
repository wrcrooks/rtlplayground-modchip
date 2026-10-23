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


/* CMD_PARSER_EXT, see CMakeLists.txt: true if the command was ours */
bool rtl_cmd_ext(void)
{
	if (cmd_compare(0, "https"))
		return parse_https();
	if (cmd_compare(0, "snmp"))
		return parse_snmp();
	return false;
}

/* ---- the switch ports, for the SNMP agent's interface tables ---- */

uint8_t rtl_port_count(void)
{
	return machine.max_port - machine.min_port + 1;
}


static uint64_t stat_read(uint8_t counter, uint8_t port)
{
	uint64_t v;

	STAT_GET(counter, port);
	reg_read_m(RTL837X_STAT_V_HIGH);
	v = (uint64_t)((uint32_t)sfr_data[0] << 24 | (uint32_t)sfr_data[1] << 16 | sfr_data[2] << 8 | sfr_data[3]) << 32;
	reg_read_m(RTL837X_STAT_V_LOW);
	return v | ((uint32_t)sfr_data[0] << 24 | (uint32_t)sfr_data[1] << 16 | sfr_data[2] << 8 | sfr_data[3]);
}


/* phys counts from 1, as the web UI's port numbers do */
bool rtl_port_read(uint8_t phys, struct rtl_port *p)
{
	if (phys < 1 || phys > rtl_port_count())
		return false;
	uint8_t i = machine.phys_to_log_port[phys - 1];

	memset(p, 0, sizeof(*p));
	if (port_names[i][0])
		strncpy(p->name, port_names[i], sizeof(p->name) - 1);
	else
		snprintf(p->name, sizeof(p->name), "Port %u", phys);
	p->sfp = machine.is_sfp[i] != 0;
	if (p->sfp) {
		p->enabled = true;
	} else {
		phy_read(i, PHY_MMD31, 0xa610);
		p->enabled = SFR_DATA_8 == 0x20;
	}

	/* As send_status(): a link bit, then a speed nibble per port */
	reg_read_m(RTL837X_REG_LINKS_STS);
	if ((sfr_data[(i / 8) + 1] >> (i % 8)) & 1) {
		uint8_t b;
		reg_read_m(i < 8 ? RTL837X_REG_LINKS : RTL837X_REG_LINKS_89);
		b = sfr_data[3 - ((i & 7) >> 1)];
		p->link = ((i & 1) ? b >> 4 : b & 0xf) + 1;
	}

	/* Counter numbers as the web UI's MIB table; 32-bit ones come in pairs, first one high */
	p->in_octets = stat_read(0, i);
	p->out_octets = stat_read(1, i);
	p->in_ucast = stat_read(2, i);
	p->in_mcast = stat_read(3, i);
	p->in_bcast = stat_read(4, i);
	p->out_ucast = stat_read(5, i);
	p->out_mcast = stat_read(6, i);
	p->out_bcast = stat_read(7, i);
	uint64_t v = stat_read(8, i);
	p->out_discards = v >> 32;
	p->in_discards = v;
	v = stat_read(48, i);
	p->in_errors = v >> 32;
	p->out_errors = v;
	return true;
}
