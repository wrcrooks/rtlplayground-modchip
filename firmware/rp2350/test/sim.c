/*
 * The RTLPlayground half of the firmware (src/rtl_*.c and the modules they
 * compile) on the host, against a simulated bridge: registers, PHYs and
 * SerDes are plain memory, transmitted frames are recorded. Prints CHECK
 * lines and the web endpoints' responses for test/run_sim.py.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bridge.h"
#include "plat.h"

struct bridge_hello bridge_hello;
struct bridge_stats bridge_stats;

static uint32_t regs[0x10000];
static uint16_t phy[16][32][64];	/* phy, dev, reg & 63 */
static uint16_t sds[2][128][256];
static uint8_t config[PLAT_CONFIG_LEN];
static uint32_t uptime;
static char console[1 << 20];
static size_t console_len;
static uint32_t requests;

#define TX_MAX 64
static uint8_t tx[TX_MAX][1600];
static uint16_t tx_len[TX_MAX];
static int n_tx;

static int failures;

static void check(int cond, const char *fmt, ...)
{
	va_list ap;

	printf("CHECK %s ", cond ? "PASS" : "FAIL");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	failures += !cond;
}

static uint32_t get32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

int bridge_request(uint8_t type, const void *p1, size_t n1, const void *p2, size_t n2,
		   void *reply, size_t rmax, size_t *rlen)
{
	uint8_t p[1700], *r = reply;
	size_t n = 0, out = 0;

	if (p1) { memcpy(p, p1, n1); n += n1; }
	if (p2) { memcpy(p + n, p2, n2); n += n2; }
	requests++;
	switch (type) {
	case REQ_REG_READ:
		if (n != 2) return ST_BAD_LENGTH;
		put32(r, regs[p[0] | p[1] << 8]);
		out = 4;
		break;
	case REQ_REG_WRITE:
		if (n != 6) return ST_BAD_LENGTH;
	{
		uint16_t a = p[0] | p[1] << 8;
		regs[a] = get32(p + 2);
		/* Engines the modules poll finish at once here */
		if (a == 0x5cac || a == 0x0418 || a == 0x0f60)	/* TBL_CTRL, I2C_CTRL, STAT_GET */
			regs[a] &= ~1u;
		else if (a == 0x53d4)			/* L2_TBL_FLUSH_CTRL */
			regs[a] = 0;
		break;
	}
	case REQ_PHY_READ: {
		if (n != 4 || p[0] > 15) return ST_BAD_ARGUMENT;
		uint16_t v = phy[p[0]][p[1] & 31][p[2] & 63];
		r[0] = v; r[1] = v >> 8;
		out = 2;
		break;
	}
	case REQ_PHY_WRITE:
		if (n != 6 || p[0] > 15) return ST_BAD_ARGUMENT;
		phy[p[0]][p[1] & 31][p[2] & 63] = p[4] | p[5] << 8;
		break;
	case REQ_SDS_READ: {
		if (n != 3 || p[0] > 1 || p[1] > 127) return ST_BAD_ARGUMENT;
		uint16_t v = sds[p[0]][p[1]][p[2]];
		r[0] = v; r[1] = v >> 8;
		out = 2;
		break;
	}
	case REQ_SDS_WRITE:
		if (n != 5 || p[0] > 1 || p[1] > 127) return ST_BAD_ARGUMENT;
		sds[p[0]][p[1]][p[2]] = p[3] | p[4] << 8;
		break;
	case REQ_SFP_READ:
		return ST_FAILED;	/* no module */
	case REQ_FLASH_READ: {
		uint16_t len = p[4] | p[5] << 8;
		memset(r, 0xff, len);
		out = len;
		break;
	}
	case REQ_TX_FRAME:
		if (n_tx < TX_MAX) {
			memcpy(tx[n_tx], p + 1, n - 1);
			tx_len[n_tx++] = n - 1;
		}
		break;
	case REQ_SET_LED:
	case REQ_FLASH_ERASE:
	case REQ_FLASH_WRITE:
		break;
	default:
		return ST_UNKNOWN;
	}
	if (out > rmax)
		out = rmax;
	if (rlen)
		*rlen = out;
	return ST_OK;
}

bool bridge_is_up(void) { return true; }

void plat_console_putc(char c)
{
	if (console_len < sizeof(console) - 1)
		console[console_len++] = c;
}

void plat_delay_ms(uint32_t ms) { (void)ms; }
uint32_t plat_random32(void) { return (uint32_t)random(); }
void plat_sys_led(uint8_t state) { (void)state; }
void plat_reboot(void) { printf("CHECK FAIL unexpected reboot\n"); exit(1); }
uint32_t plat_uptime_s(void) { return uptime; }

void plat_config_read(uint32_t off, uint8_t *dst, uint32_t len)
{
	memcpy(dst, config + off, len);
}

bool plat_config_write(const uint8_t *src, uint32_t len)
{
	memset(config, 0xff, sizeof(config));
	memcpy(config, src, len);
	return true;
}

static int dhcp_starts;
void plat_dhcp_start(void) { dhcp_starts++; }
void plat_dhcp_stop(void) { }

static void body(const char *path)
{
	uint32_t r0 = requests;
	bool ok = rtl_http_get((char *)path);
	uint32_t n = slen;
	static uint8_t buf[8192];

	memcpy(buf, outbuf, slen);
	while (cont_len)
		n += rtl_http_cont(buf + n, cont_len);
	check(ok && n > 12 && !memcmp(buf, "HTTP/1.1 200", 12), "GET %s answers 200", path);
	printf("COST %s %u requests\n", path, requests - r0);
	printf("BODY %s %u\n", path, n);
	fwrite(buf, 1, n, stdout);
	printf("\n");
}

static void cmd(const char *c)
{
	static char b[256];

	strcpy(b, c);
	rtl_http_cmd(b);
	outbuf[slen] = 0;
}

static void ticks_run(int n)
{
	while (n--) {
		ticks++;
		rtl_tick();
		if (!(ticks % 200)) {
			uptime++;
			rtl_second();
		}
	}
}

int main(void)
{
	alarm(20);	/* a module stuck polling a register would hang here */
	memset(config, 0xff, sizeof(config));
	regs[0x0000] = 0x83720000;	/* RTL837X_REG_CHIP_ID */

	bridge_hello.version = 1;
	bridge_hello.chip_id = 0x83720000;
	memcpy(bridge_hello.mac, "\x06\x0f\x00\xdd\x3b\x3e", 6);
	bridge_hello.flash_size = 0x80000;
	strcpy(bridge_hello.machine, "SWGT024 V2.0 Unmanaged");

	rtl_boot();
	rtl_bridge_up();
	const uint8_t *ip = (const uint8_t *)uip_hostaddr;
	check(ip[0] == 192 && ip[1] == 168 && ip[2] == 10 && ip[3] == 247, "default config replayed: ip %u.%u.%u.%u",
	      ip[0], ip[1], ip[2], ip[3]);
	check(config[0] == 'i', "blank config sector got the default config");
	check(!strcmp(hostname, "RTLPlayground-dd3b3e"), "hostname from the MAC: %s", hostname);
	check(!memcmp(rtl_mac(), bridge_hello.mac, 6), "MAC from HELLO");
	check(!strstr(console, "ERROR"), "no errors replaying the config");

	static const char *routes[] = {
		"/status.json", "/information.json", "/vlan.json?vid=1", "/counters.json?port=1",
		"/eee.json", "/bandwidth.json", "/storm.json", "/l2.json?idx=0", "/mirror.json",
		"/isolation.json", "/mtu.json", "/lag.json", "/stp.json", "/lldp.json", "/lacp.json",
		"/vlanlist", "/config", "/cmd_log",
	};
	for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
		body(routes[i]);
	check(!rtl_http_get("/nothing.json"), "unknown route left to the caller");

	cmd("hostname sim-switch");
	check(!strcmp(hostname, "sim-switch") && strstr((char *)outbuf, "200 OK"), "POST /cmd: hostname");
	cmd("ip 10.1.2.3");
	check(ip[0] == 10 && ip[3] == 3, "POST /cmd: ip");
	cmd("no-such-command");
	check(!memcmp(outbuf, "HTTP/1.1 400 NO", 15), "POST /cmd: a bad command answers 400");
	cmd("ip dhcp");
	check(dhcp_starts == 1, "POST /cmd: ip dhcp starts lwIP's client");

	check(rtl_password_ok("1234") && !rtl_password_ok("12345") && !rtl_password_ok("123"),
	      "default password 1234");

	/* An ARP request as the CPU port delivers it: CPU tag and 802.1Q tag after the MACs */
	uint8_t f[64] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 1, 2, 3, 4, 5, 6,
			  0x88, 0x99, 0x04, 0x00, 0x00, 0x00, 0x00, 0x03,	/* CPU tag, port 3 */
			  0x81, 0x00, 0x00, 0x01,				/* VLAN 1 */
			  0x08, 0x06 };
	uint16_t vlan, off = rtl_rx_frame(f, sizeof(f), &vlan);
	check(off == 24 && vlan == 1 && f[12] == 0xff && f[18] == 1 && f[23] == 6 && f[24] == 0x08,
	      "ARP for lwIP: EtherType at %u, MACs in front of it", off);
	f[23] = 0x02;
	f[26] = 0x08;
	uint8_t g[64];
	memcpy(g, f, sizeof(g));
	memcpy(g, "\xff\xff\xff\xff\xff\xff\x01\x02\x03\x04\x05\x06\x88\x99\x04\x00\x00\x00\x00\x03\x81\x00\x00\x02\x08\x00", 26);
	check(rtl_rx_frame(g, sizeof(g), &vlan) == 0, "IPv4 outside the management VLAN dropped");

	/* LLDP and STP go out CPU-tagged, never with a management VLAN tag */
	cmd("lldp on");
	cmd("stp on");
	n_tx = 0;
	ticks_run(200 * 35);
	int tagged = 0, cpu = 0;
	for (int i = 0; i < n_tx; i++) {
		cpu += tx[i][12] == 0x88 && tx[i][13] == 0x99;
		tagged += tx[i][12] == 0x81 && tx[i][13] == 0x00;
	}
	check(n_tx > 0 && cpu == n_tx && !tagged, "L2 protocol frames sent: %d, CPU-tagged: %d", n_tx, cpu);

	/* An IP frame from lwIP gets the management VLAN tag */
	n_tx = 0;
	uint8_t ipf[60] = { 1, 2, 3, 4, 5, 6, 6, 0x0f, 0, 0xdd, 0x3b, 0x3e, 0x08, 0x00 };
	rtl_tx_frame(ipf, sizeof(ipf));
	check(n_tx == 1 && tx_len[0] == 64 && tx[0][12] == 0x81 && tx[0][15] == 1 && tx[0][16] == 0x08,
	      "IP frame tagged for VLAN 1");

	/* Syslog over the uIP UDP shim */
	cmd("syslog ip 10.1.2.9");
	cmd("syslog on");
	struct rtl_udp_info info = { 0 };
	int s = -1;
	for (int i = 0; i < RTL_UDP_CONNS; i++) {
		rtl_udp_info(i, &info);
		if (info.rport == 514) {
			s = i;
			break;
		}
	}
	check(s >= 0 && info.ripaddr[0] == 10 && info.ripaddr[3] == 9 && info.lport, "syslog connection opened");
	cmd("version");	/* prints, and so logs */
	if (s >= 0) {
		uint8_t *d;
		uint16_t n = rtl_udp_poll(s, &d);
		check(n > 4 && !memcmp(d, "<14>", 4), "syslog datagram: %.*s", n > 60 ? 60 : n, d);
	}

	/* Config writes through the flash API land in our sector */
	cmd("passwd secret");
	check(rtl_password_ok("secret"), "passwd changes the password");

	printf("REQUESTS %u\n", requests);
	printf("CONSOLE %zu\n", console_len);
	fwrite(console, 1, console_len, stdout);
	printf("\nDONE %d\n", failures);
	return failures ? 1 : 0;
}
