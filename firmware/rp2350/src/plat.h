#ifndef _PLAT_H_
#define _PLAT_H_

/*
 * The seam between the two halves of the firmware. RTLPlayground's sources
 * and the files around them (rtl_*.c) compile with test/sdcc_shim.h forced
 * in, which redefines words like __naked and __data that the Pico SDK and
 * lwIP headers use themselves. So neither half includes the other's headers;
 * everything they share is declared here in plain C.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- implemented by the platform half (Pico SDK, lwIP) ---- */

void plat_console_putc(char c);
void plat_delay_ms(uint32_t ms);		/* keeps the bridge link serviced */
uint32_t plat_random32(void);
void plat_sys_led(uint8_t state);		/* SYS_LED_* */
void plat_reboot(void);				/* the switch and the RP2350 */
uint32_t plat_uptime_s(void);

/* The saved configuration: one 4 KiB sector of the RP2350's flash */
#define PLAT_CONFIG_LEN		0x1000
void plat_config_read(uint32_t off, uint8_t *dst, uint32_t len);
bool plat_config_write(const uint8_t *src, uint32_t len);	/* replaces the whole sector */

/* lwIP's DHCP client behind RTLPlayground's dhcp_start()/dhcp_stop() */
void plat_dhcp_start(void);
void plat_dhcp_stop(void);

/* ---- implemented by the RTLPlayground half (rtl_*.c) ---- */

extern volatile uint32_t ticks;			/* 200 Hz, SYS_TICK_HZ */
extern uint16_t management_vlan;
extern char hostname[24];
extern uint16_t uip_hostaddr[2], uip_draddr[2], uip_netmask[2];	/* network order in memory */

const uint8_t *rtl_mac(void);			/* uip_ethaddr, the "mac" command can change it */
void rtl_boot(void);
void rtl_bridge_up(void);			/* after each HELLO */
void rtl_bridge_down(void);
void rtl_tick(void);				/* each SYS_TICK */
void rtl_second(void);
void rtl_poll(void);				/* console commands */
void rtl_console_input(char c);
void rtl_on_link(const uint8_t *links);	/* MSG_LINK's 12 payload bytes */
void rtl_on_sfp(uint8_t sfp_pins);

/*
 * A frame from the CPU port: CPU tag after the source MAC, then an 802.1Q
 * tag. The L2 protocols take theirs; for anything for the IP stack, returns
 * the offset of the EtherType (the frame is then rewritten so that dst, src
 * end right in front of it) or 0.
 */
uint16_t rtl_rx_frame(uint8_t *frame, uint16_t len, uint16_t *vlan);
/* An Ethernet frame out through the bridge, tagged for the management VLAN */
void rtl_tx_frame(const uint8_t *frame, uint16_t len);

/* DHCP state shown by the command line, from lwIP */
void rtl_dhcp_update(uint8_t state, uint16_t timer_s, const uint8_t *dns);

/* uIP's UDP API for syslog.c, dns.c and ntp.c, over lwIP */
#define RTL_UDP_CONNS		4
#define RTL_UDP_BUF		600
struct rtl_udp_info {
	uint16_t lport, rport;			/* host order, 0: unused */
	uint8_t ripaddr[4];
};
void rtl_udp_info(int i, struct rtl_udp_info *info);
uint16_t rtl_udp_poll(int i, uint8_t **data);			/* -> bytes to send */
uint16_t rtl_udp_input(int i, const uint8_t *p, uint16_t len, uint8_t **data);

/* The web server's view of RTLPlayground: page_impl.c writes into outbuf */
#define RTL_SESSION_ID_LEN	12
extern uint8_t outbuf[];
extern uint16_t slen;
extern uint16_t cont_len;
extern uint32_t cont_addr;
extern uint16_t session_timeout;		/* seconds */

bool rtl_http_get(char *path);			/* page_impl.c routes; false: not one */
void rtl_http_cmd(char *body);			/* POST /cmd */
bool rtl_password_ok(const char *urlencoded);
uint16_t rtl_http_cont(uint8_t *dst, uint16_t max);	/* the rest after outbuf, cont_len */

#endif
