/*
 * RTLPlayground's main loop, CPU-port dispatch and web routes, as the
 * RP2350 runs them: rtlplayground.c's main(), handle_tick(), handle_rx()
 * and httpd.c's GET table, minus what the 8051 bridge keeps (chip init,
 * SerDes, the NIC). Compiled with sdcc_shim.h, like the modules.
 */

#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_port.h"
#include "rtl837x_stp.h"
#include "rtl837x_lacp.h"
#include "rtl837x_igmp.h"
#include "rtl837x_lldp.h"
#include "rtl837x_bandwidth.h"
#include "rtl837x_cabletest.h"
#include "rtl837x_flash.h"
#include "rtl837x_leds.h"
#include "rtl837x_pins.h"
#include "machine.h"
#include "cmd_parser.h"
#include "cmd_editor.h"
#include "sfp.h"
#include "dhcp.h"
#include "dns.h"
#include "ntp.h"
#include "syslog.h"
#include "uip/uip.h"
#include "httpd/page_impl.h"

#include "bridge.h"
#include "plat.h"

#define STP_TICK_STEP	(SYS_TICK_HZ / STP_HZ)
#define STP_CATCH_UP	8
#define SFP_READ_TRIES	3

/* 802.1Q */
#define DOT_1Q_TAG_SIZE	4

extern __code const struct machine machine;
extern __xdata struct machine_runtime machine_detected;
extern __code const uint8_t * __code const hex;
extern __xdata struct flash_region_t flash_region;
extern __xdata uint8_t flash_buf[FLASH_BUF_SIZE];
extern __xdata uint8_t igmpEnabled;
extern __xdata uint8_t lacpEnabled;
extern __xdata uint8_t cmd_capture;
extern __xdata struct dhcp_state dhcp_state;
extern __xdata char passwd[21];

__xdata uint8_t stp_tick_last;
static bool up;
static uint8_t links_last[12];
static bool button_last;
static uint32_t button_since;
static uint8_t sfp_read_due[2];		/* seconds until the module's EEPROM is read, 0: none */
static uint8_t sfp_read_tries[2];
static uint16_t short_parsed;
static uint8_t tx_frame[UIP_CONF_BUFFER_SIZE + DOT_1Q_TAG_SIZE];

__code const uint8_t * __code const greeting = "\nRTLPlayground on the mod chip\n";


const uint8_t *rtl_mac(void)
{
	return uip_ethaddr.addr;
}


void rtl_boot(void)
{
	dhcp_state.state = DHCP_OFF;
	sbuf_ptr = 0;
	sfp_pins_last = 0x33;	/* LOS and no module, both slots */
	sfp_speed[0] = sfp_speed[1] = SFP_SPEED_AUTO;
	syslog_init();
	dns_init();
	ntp_init();
	cmd_editor_init();
	print_string(greeting);
}


/* As set_hostname_default() in boot.c */
static void hostname_default(void)
{
	if (hostname[0])
		return;
	strcpy(hostname, "RTLPlayground-");
	for (uint8_t i = 0; i < 3; i++) {
		hostname[14 + 2 * i] = hex[uip_ethaddr.addr[3 + i] >> 4];
		hostname[15 + 2 * i] = hex[uip_ethaddr.addr[3 + i] & 0xf];
	}
	hostname[20] = NUL;
}


/*
 * The part of main() after the chip is up, run after every HELLO: the bridge
 * has just booted with forwarding defaults, or we have, and either way the
 * registers we own need their configuration again.
 */
void rtl_bridge_up(void)
{
	up = false;
	machine_detected.isRTL8373 = !!(bridge_hello.flags & HELLO_RTL8373);
	machine_detected.isN = !!(bridge_hello.flags & HELLO_N_VARIANT);
	if (machine.isRTL8373 != machine_detected.isRTL8373)
		print_string("INCORRECT MACHINE!\n");
	memcpy(uip_ethaddr.addr, bridge_hello.mac, 6);
	print_string("MAC: ");
	for (uint8_t i = 0; i < 6; i++) {
		if (i)
			write_char(':');
		print_byte(uip_ethaddr.addr[i]);
	}
	write_char('\n');

	uip_ipaddr(&uip_hostaddr, 192, 168, 2, 2);
	uip_ipaddr(&uip_draddr, 192, 168, 2, 22);
	uip_ipaddr(&uip_netmask, 255, 255, 255, 0);

	stp_enabled = 0;
	stp_defaults();
	lacpEnabled = 0;
	lacp_init();
	igmp_setup();
	bandwidth_setup();
	lldp_init();
	management_vlan = 1;
	hostname[0] = NUL;

	flash_region.addr = CONFIG_START;
	flash_region.len = 0x100;
	flash_read_bulk(flash_buf);
	if (flash_buf[0] == 0xff) {
		print_string("Config sector is blank, writing the default config\n");
		flash_default_config();
	}
	execute_config();
	port_l2_static_mgmt(uip_ethaddr.addr, management_vlan, false);
	hostname_default();

	stp_tick_last = ticks;
	up = true;
	set_sys_led_state(SYS_LED_ON);
	print_cmd_prompt();
}


void rtl_bridge_down(void)
{
	up = false;
}


void rtl_tick(void)
{
	if (!up)
		return;
	if (stp_enabled) {
		uint8_t n = STP_CATCH_UP;
		while (n-- && TICKS_DUE(stp_tick_last, STP_TICK_STEP)) {
			stp_tick_last += STP_TICK_STEP;
			stp_timers();
		}
	}
	if (lacpEnabled)
		lacp_timers();
	lldp_tick();
	cabletest_tick();
}


/* As handle_button() in rtlplayground.c, timed by our clock */
static void handle_button(void)
{
	if (machine.reset_pin == GPIO_NA)
		return;

	bool pressed = !gpio_pin_test(machine.reset_pin);
	if (pressed == button_last)
		return;
	print_string(pressed ? "Button pressed\n" : "Button released\n");
	uint32_t held = plat_uptime_s() - button_since;
	button_last = pressed;
	button_since = plat_uptime_s();
	if (pressed) {
		set_sys_led_state(SYS_LED_SLOW);
		return;
	}
	if (held > 10) {
		print_string(">10s button detected; reverting to default settings:\n");
		flash_default_config();
		print_string("Now resetting...\n");
		reset_chip();
	} else if (held > 3) {
		print_string(">3s button detected; resetting chip...\n");
		reset_chip();
	} else {
		print_string("Short button press detected; no action.\n");
		set_sys_led_state(SYS_LED_ON);
	}
}


/* The module part of sfp_module_read() in sfp.c; the 8051 sets the SerDes */
static bool sfp_read_module(uint8_t sfp)
{
	if (!sfp_read_block(sfp, 11, 2))
		return false;
	print_string("  Rate: "); print_byte(sfp_buf[1]);
	print_string("  Encoding: "); print_byte(sfp_buf[0]);
	print_string("  Module: ");
	if (!sfp_print_info(sfp))
		return false;
	write_char('\n');
	if (!sfp_read_block(sfp, 92, 1))
		return false;
	sfp_options[sfp] = sfp_buf[0];
	if (!sfp_get_info(sfp))
		return false;
	sfp_apply_quirks(sfp);
	return true;
}


void rtl_second(void)
{
	if (!up)
		return;
	handle_button();
	for (uint8_t sfp = 0; sfp < 2; sfp++) {
		if (!sfp_read_due[sfp] || --sfp_read_due[sfp])
			continue;
		if (sfp_read_module(sfp))
			continue;
		if (++sfp_read_tries[sfp] < SFP_READ_TRIES)
			sfp_read_due[sfp] = 1;
		else
			print_string("SFP: the module's EEPROM doesn't answer\n");
	}
}


void rtl_console_input(char c)
{
	sbuf[sbuf_ptr] = c;
	sbuf_ptr = (sbuf_ptr + 1) & SBUF_MASK;
}


void rtl_poll(void)
{
	cmd_edit();
	if (cmd_available) {
		cmd_available = 0;
		cmd_tokenize();
		if (err_status == ERR_OK)
			cmd_parser();
		print_cmd_prompt();
	}
}


void rtl_on_link(const uint8_t *links)
{
	if (!memcmp(links, links_last, sizeof(links_last)))
		return;
	print_string("\n<new link: ");
	print_byte(links[8]);
	for (int8_t i = 7; i >= 4; i--)
		print_byte(links[i]);
	print_string(", was ");
	print_byte(links_last[8]);
	for (int8_t i = 7; i >= 4; i--)
		print_byte(links_last[i]);
	print_string(">\n");
	memcpy(links_last, links, sizeof(links_last));
}


/* Bits as sfp_pins_last in sfp.c: per slot, 0x1 no module, 0x2 LOS */
void rtl_on_sfp(uint8_t pins)
{
	for (uint8_t sfp = 0; sfp < machine.n_sfp && sfp < 2; sfp++) {
		uint8_t now = pins >> (sfp << 2), was = sfp_pins_last >> (sfp << 2);

		if ((was & 1) && !(now & 1)) {
			print_string("\n<MODULE INSERTED>  Slot: "); write_char('1' + sfp); write_char('\n');
			sfp_read_due[sfp] = 1 + SFP_WAKE_TICKS / SYS_TICK_HZ;
			sfp_read_tries[sfp] = 0;
		} else if (!(was & 1) && (now & 1)) {
			print_string("\n<MODULE REMOVED>  Slot: "); write_char('1' + sfp); write_char('\n');
			sfp_read_due[sfp] = 0;
			sfp_module_vendor[sfp][0] = sfp_module_model[sfp][0] = sfp_module_serial[sfp][0] = NUL;
		}
		if ((was & 2) && !(now & 2)) {
			print_string("\n<SFP-RX OK>  Slot: "); write_char('1' + sfp); write_char('\n');
		} else if (!(was & 2) && (now & 2)) {
			print_string("\n<SFP-RX LOS>  Slot: "); write_char('1' + sfp); write_char('\n');
		}
	}
	sfp_pins_last = pins;
}


/*
 * A frame for the switch: the bridge adds the NIC descriptor. Frames in the
 * management VLAN get an 802.1Q tag, as tcpip_output() did on the 8051,
 * except CPU-tagged ones, which the ASIC wants with its tag after the MACs.
 */
void rtl_tx_frame(const uint8_t *frame, uint16_t len)
{
	static const uint8_t flags = 0x07;	/* all checksums */
	const uint8_t *f = frame;

	if (len < ETHER_HEADER_SIZE || len > UIP_CONF_BUFFER_SIZE)
		return;
	if (management_vlan && !(frame[12] == 0x88 && frame[13] == 0x99)) {
		memcpy(tx_frame, frame, 12);
		tx_frame[12] = 0x81;
		tx_frame[13] = 0x00;
		tx_frame[14] = management_vlan >> 8;
		tx_frame[15] = management_vlan;
		memcpy(tx_frame + 16, frame + 12, len - 12);
		f = tx_frame;
		len += DOT_1Q_TAG_SIZE;
	}
	bridge_request(REQ_TX_FRAME, &flags, 1, f, len, NULL, 0, NULL);
}


/* The L2 protocols build their frames in uip_buf behind room for the NIC descriptor */
void tcpip_output(void)
{
	rtl_tx_frame(uip_buf + RTL_FRAME_DESC_SIZE, uip_len);
}


#define ETH_TYPE_OFFSET	(12 + RTL_TAG_SIZE + VLAN_TAG_SIZE)

/* As handle_rx() in rtlplayground.c, for one frame */
uint16_t rtl_rx_frame(uint8_t *frame, uint16_t len, uint16_t *vlan)
{
	if (!up || len < ETH_TYPE_OFFSET + 2 || len > UIP_CONF_BUFFER_SIZE)
		return 0;
	memcpy(uip_buf, frame, len);
	uip_len = len;
	*vlan = ((uip_buf[22] << 8) | uip_buf[23]) & 0x0fff;

	if ((stp_enabled || lacpEnabled) && uip_buf[0] == 0x01 && uip_buf[1] == 0x80
	    && uip_buf[2] == 0xc2 && uip_buf[3] == 0x00 && uip_buf[4] == 0x00) {
		if (stp_enabled && uip_buf[5] == 0x00) {
			stp_in();
			if (uip_len) {
				print_string("STP TX\n");
				tcpip_output();
			}
		} else if (lacpEnabled && uip_buf[5] == 0x02) {
			lacp_in();
		}
		return 0;
	}
	if (igmpEnabled && uip_buf[0] == 0x01 && uip_buf[1] == 0x00 && uip_buf[2] == 0x5e
	    && igmp_packet_handler()) {
		if (uip_len)
			tcpip_output();
		return 0;
	}
	uint16_t type = (frame[ETH_TYPE_OFFSET] << 8) | frame[ETH_TYPE_OFFSET + 1];
	if (type != 0x0806 && type != 0x0800)
		return 0;
	if (type == 0x0800 && management_vlan && management_vlan != *vlan)
		return 0;
	/* Drop the CPU and VLAN tags: dst and src move up to the EtherType */
	memmove(frame + ETH_TYPE_OFFSET - 12, frame, 12);
	return ETH_TYPE_OFFSET;
}


void rtl_dhcp_update(uint8_t state, uint16_t timer_s, const uint8_t *dns)
{
	dhcp_state.state = state;
	dhcp_state.dhcp_timer = timer_s;
	if (dns)
		memcpy(dhcp_state.dns, dns, 4);
	else
		memset(dhcp_state.dns, 0, 4);
}


/* cmd_parser.c starts and stops DHCP through these, renamed to rtlp_dhcp_*()
 * by CMakeLists.txt so as not to collide with lwIP's */
void dhcp_start(void)
{
	plat_dhcp_start();
}


void dhcp_stop(void)
{
	plat_dhcp_stop();
}

/* ---- the web routes ---- */

static bool is_word(__xdata uint8_t *s, __code const uint8_t *w)
{
	uint8_t u, c;

	while (1) {
		u = *s++;
		c = *w++;
		if (c == NUL)
			return u == NUL || u == ' ' || u == '\t' || u == ':' || u == '?' || u == '='
				|| u == '\n' || u == '\r';
		if (c != u)
			return false;
	}
}


static void parse_short(__xdata uint8_t *p)
{
	uint8_t c;

	short_parsed = 0;
	while ((c = *p++ - '0') <= 9) {
		if (short_parsed > 6552)
			short_parsed = 0xffff;
		else
			short_parsed = short_parsed * 10 + c;
	}
}


static void send_bad_request(void)
{
	slen = strtox(outbuf, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Type: text/html\r\n\r\n"
			      "<!DOCTYPE HTML PUBLIC>\n<title>400 Bad Request</title>\n<h1>Bad Request</h1>\n");
}


/* httpd.c's GET table for the JSON and text endpoints; q is the request target */
bool rtl_http_get(char *path)
{
	__xdata uint8_t *q = (__xdata uint8_t *)path;

	cont_len = 0;
	slen = 0;
	if (!strcmp(path, "/status.json")) {
		send_status();
	} else if (!strcmp(path, "/information.json")) {
		send_basic_info();
	} else if (is_word(q, "/vlan.json")) {
		parse_short(q + 15);
		send_vlan(short_parsed);
	} else if (is_word(q, "/counters.json")) {
		if (strlen(path) < 21 || send_counters(q[20] - '0'))
			send_bad_request();
	} else if (is_word(q, "/eee.json")) {
		send_eee();
	} else if (is_word(q, "/bandwidth.json")) {
		send_bandwidth();
	} else if (is_word(q, "/storm.json")) {
		send_storm();
	} else if (is_word(q, "/l2.json")) {
		parse_short(q + 13);
		send_l2(short_parsed);
	} else if (is_word(q, "/l2_del.json")) {
		parse_short(q + 17);
		l2_delete(short_parsed);
	} else if (is_word(q, "/mirror.json")) {
		send_mirror();
	} else if (is_word(q, "/isolation.json")) {
		send_isolation();
	} else if (is_word(q, "/mtu.json")) {
		send_mtu();
	} else if (is_word(q, "/lag.json")) {
		send_lag();
	} else if (is_word(q, "/stp.json")) {
		send_stp();
	} else if (is_word(q, "/lldp.json")) {
		send_lldp();
	} else if (is_word(q, "/lacp.json")) {
		send_lacp();
	} else if (is_word(q, "/vlanlist")) {
		send_vlanlist();
	} else if (is_word(q, "/config")) {
		send_config();
	} else if (is_word(q, "/cmd_log")) {
		send_cmd_log();
	} else if (is_word(q, "/cmd_log_clear")) {
		clear_command_history();
		send_mtu();	/* a dummy response, as httpd.c sends */
	} else {
		return false;
	}
	return true;
}


/* More of a response than outbuf held: send_config() leaves the rest in flash */
uint16_t rtl_http_cont(uint8_t *dst, uint16_t max)
{
	uint16_t n = cont_len < max ? cont_len : max;

	flash_region.addr = cont_addr;
	flash_region.len = n;
	flash_read_bulk(dst);
	cont_addr += n;
	cont_len -= n;
	return n;
}


/* As run_cmd_body() in httpd.c */
void rtl_http_cmd(char *body)
{
	uint16_t hdr_len = strtox(outbuf, HTTP_RESPONCE_TXT);

	cont_len = 0;
	slen = hdr_len;
	cmd_capture = 1;
	execute_commands((__xdata uint8_t *)body);
	if (cmd_capture == 2)
		slen += strtox(outbuf + slen, CMD_TRUNCATED);
	cmd_capture = 0;
	if (err_status == ERR_OK && slen == hdr_len)
		slen += strtox(outbuf + slen, "OK\n");
	if (err_status != ERR_OK) {
		outbuf[9] = '4';
		outbuf[10] = '0';
		outbuf[11] = '0';
		outbuf[13] = 'N';
		outbuf[14] = 'O';
	}
}


static uint8_t hexval(uint8_t c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return 0;
}


/* The form value of pwd= against passwd, as is_url_word_x() in httpd.c */
bool rtl_password_ok(const char *v)
{
	const char *s = passwd;
	uint8_t u;

	while (1) {
		u = *v++;
		if (!*s)
			return u == NUL || u == '&' || u == ' ' || u == '\t' || u == ':' || u == '?'
				|| u == '=' || u == '\r' || u == '\n';
		if (u == '%' && v[0] && v[1]) {
			u = hexval(v[0]) << 4 | hexval(v[1]);
			v += 2;
		} else if (u == '+') {
			u = ' ';
		}
		if (u != (uint8_t)*s++)
			return false;
	}
}
