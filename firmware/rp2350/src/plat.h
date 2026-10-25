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
void plat_watchdog_test(void);			/* "watchdog test": stop, so the watchdog restarts us */
void plat_bridge_print(void);			/* link state and counters, for "bridge" */
void plat_bridge_log(void);			/* "bridge log": the 8051's recent output */
void plat_bridge_hold(bool on);			/* "bridge hold on|off" */
void plat_bridge_raw(void);			/* "bridge raw": the first bytes after power-up */
void plat_bridge_probe(void);			/* "bridge probe": activity on the UART pins */
uint32_t plat_uptime_s(void);
uint32_t plat_ms(void);

/* The saved configuration: one 4 KiB sector of the RP2350's flash */
#define PLAT_CONFIG_LEN		0x1000
void plat_config_read(uint32_t off, uint8_t *dst, uint32_t len);
bool plat_config_write(const uint8_t *src, uint32_t len);	/* replaces the whole sector */

/* lwIP's DHCP client behind RTLPlayground's dhcp_start()/dhcp_stop() */
void plat_dhcp_start(void);
void plat_dhcp_stop(void);

/* HTTPS on port 443 (tls.c) */
bool plat_https_enable(bool on);
bool plat_https_enabled(void);
bool plat_https_newcert(void);
void plat_https_status(char *fingerprint, size_t len);

/* The SNMP agent's settings (snmp.c); set() with NULL clears, false: too long */
enum { PLAT_SNMP_ENABLED, PLAT_SNMP_COMMUNITY, PLAT_SNMP_LOCATION, PLAT_SNMP_CONTACT };
bool plat_snmp_set(int what, const char *value);
const char *plat_snmp_get(int what);	/* ENABLED: non-NULL when on */

/*
 * T3: register access over the switch's I2C slave interface (t3.c), as an
 * alternative to the bridge's REG_READ/REG_WRITE. The bus comes up with the
 * switch's power; register access goes over it only once enabled.
 */
struct plat_t3 {
	bool up;			/* the bus is configured */
	bool enabled;			/* register access goes over it */
	uint16_t khz;
	uint32_t reads, writes, errors;
};
void plat_t3_enable(bool on, uint16_t khz);	/* khz 0: keep the speed */
void plat_t3_status(struct plat_t3 *s);
bool plat_t3_enabled(void);
bool plat_t3_read(uint16_t reg, uint32_t *v);	/* counted; false: use the bridge */
bool plat_t3_write(uint16_t reg, uint32_t v);
bool plat_t3_raw_read(uint16_t reg, uint32_t *v);	/* whether enabled or not, for "t3 test" */
bool plat_t3_raw_write(uint16_t reg, uint32_t v);

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

/* The switch ports, for the SNMP agent (rtl_ext.c) */
struct rtl_port {
	char name[32];
	uint8_t link;			/* 0 down, else 1 + the speed code: 10M, 100M, 1G, 500M, 10G, 2.5G, 5G */
	uint16_t max_mbps;		/* the port's top speed */
	bool enabled;
	bool sfp;
	uint64_t in_octets, out_octets, in_ucast, in_mcast, in_bcast, out_ucast, out_mcast, out_bcast;
	uint32_t in_discards, out_discards, in_errors, out_errors;
};
uint8_t rtl_port_count(void);
bool rtl_port_read(uint8_t phys, struct rtl_port *p);	/* phys from 1 */

/* ---- for the dashboard and the SNMP agent (rtl_metrics.c) ---- */

#define RTL_MAX_PORTS		10

/* All ports, front-panel order, counters in one bridge request; returns how many */
uint8_t rtl_ports_read(struct rtl_port *p, uint8_t max);

struct rtl_status {
	int16_t chip_temp;		/* 1/128 degC, TM_RESULT */
	uint16_t mac_entries;		/* learned addresses, all ports */
	bool stp_on;
	uint8_t stp_root_port;		/* STP entity, 0xff: this switch is the root */
	uint32_t stp_tc;		/* topology changes */
	uint32_t stp_root_changes;
	bool lacp_on;
	uint8_t lacp_ports_up;		/* ports collecting and distributing */
	uint32_t lacp_changes;		/* partner or sync changes */
	bool igmp_on;
	uint16_t igmp_groups;		/* groups with members, as snooped */
	uint32_t igmp_joins, igmp_leaves;
};
void rtl_status_read(struct rtl_status *s);

struct rtl_sfp {
	bool present, los, ddm;
	char vendor[17], model[17];
	int16_t temp;			/* SFF-8472 diagnostics: 1/256 degC */
	uint16_t vcc;			/* 100 uV */
	uint16_t bias;			/* 2 uA */
	uint16_t tx_power, rx_power;	/* 0.1 uW */
};
uint8_t rtl_sfp_count(void);
void rtl_sfp_read(uint8_t slot, struct rtl_sfp *d);	/* reads the module's diagnostics */

struct rtl_link_event {
	uint32_t seq;
	uint32_t t_ms;
	uint8_t port;			/* front-panel number */
	uint8_t link;			/* 0 down, else as rtl_port.link */
};
uint8_t rtl_link_events(struct rtl_link_event *out, uint8_t max);	/* oldest first */

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
