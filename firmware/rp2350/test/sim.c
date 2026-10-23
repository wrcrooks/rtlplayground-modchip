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

#include "machine.h"
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
static uint32_t vlan_tbl[0x1000][3];	/* the VLAN table behind TBL_CTRL */
static bool old_bridge;			/* answer as a bridge from before TBL_READ */
static int sds_configs;
static uint8_t sds_last[2];
static bool sfp_module;
static int stat_reads;

/* The MIB counters: a known value for each port and counter */
static uint64_t stat_value(uint8_t port, uint8_t cnt)
{
	return (uint64_t)(port * 1000 + cnt) << 32 | (uint32_t)((cnt + 1) * 1000003u * (port + 1));
}

/* STAT_GET: REG_WRITE(STAT_GET, 0, 0, cnt >> 3, cnt << 5 | port << 1 | 1) */
static void stat_get(uint32_t v)
{
	uint8_t cnt = ((v >> 8) & 0xff) << 3 | (v & 0xff) >> 5, port = (v >> 1) & 0xf;
	uint64_t x = stat_value(port, cnt);

	regs[0x0f64] = x >> 32;
	regs[0x0f68] = x;
	regs[0x0f60] = v & ~1u;
}

/* The table engine, enough of it for VLAN entries */
static void tbl_ctrl(uint32_t v)
{
	uint16_t entry = v >> 16;

	if (((v >> 8) & 0xff) == 0x03 && (v & 1) && entry < 0x1000) {
		if (v & 2) {
			vlan_tbl[entry][0] = regs[0x5cb8];
			vlan_tbl[entry][1] = regs[0x5cbc];
			vlan_tbl[entry][2] = regs[0x5cc0];
		} else {
			regs[0x5ccc] = vlan_tbl[entry][0];
			regs[0x5cd0] = vlan_tbl[entry][1];
			regs[0x5cd4] = vlan_tbl[entry][2];
		}
	}
	regs[0x5cac] = v & ~1u;
}

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

/* A register write as the chip takes it, whichever path it came by */
static void sim_reg_write(uint16_t a, uint32_t v)
{
	regs[a] = v;
	if (a == 0x5cac)
		tbl_ctrl(regs[a]);
	if (a == 0x0f60)
		stat_get(regs[a]);
	/* Engines the modules poll finish at once here */
	if (a == 0x5cac || a == 0x0418)		/* TBL_CTRL, I2C_CTRL */
		regs[a] &= ~1u;
	else if (a == 0x53d4)			/* L2_TBL_FLUSH_CTRL */
		regs[a] = 0;
}

/* T3: the same registers over I2C, behaving as told */
enum { T3_GOOD, T3_DEAD, T3_DROPS_WRITES, T3_FAILS } t3_mode = T3_DEAD;
static uint32_t t3_ops, bridge_reg_ops;

bool t3_bus_up(void) { return true; }
void t3_bus_speed(uint16_t khz) { (void)khz; }

bool t3_bus_read(uint16_t reg, uint32_t *v)
{
	if (t3_mode == T3_DEAD || t3_mode == T3_FAILS)
		return false;
	t3_ops++;
	*v = regs[reg];
	return true;
}

bool t3_bus_write(uint16_t reg, uint32_t v)
{
	if (t3_mode == T3_DEAD || t3_mode == T3_FAILS)
		return false;
	t3_ops++;
	if (t3_mode == T3_GOOD)
		sim_reg_write(reg, v);
	return true;	/* T3_DROPS_WRITES: acknowledged, without effect */
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
		bridge_reg_ops++;
		put32(r, regs[p[0] | p[1] << 8]);
		out = 4;
		break;
	case REQ_REG_WRITE:
		if (n != 6) return ST_BAD_LENGTH;
		sim_reg_write(p[0] | p[1] << 8, get32(p + 2));
		bridge_reg_ops++;
		break;
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
		if (!sfp_module || p[0] != 0 || p[2] > 16)
			return ST_FAILED;
		memset(r, ' ', p[2]);
		if (p[1] == 11 && p[2] >= 2) {
			r[0] = 0x03;	/* encoding */
			r[1] = 0x0d;	/* 1.3 GBd: 1G */
		}
		if (p[1] == 92)
			r[0] = 0x40;	/* diagnostics implemented */
		if (p[1] == (0x80 | 96) && p[2] == 10) {
			static const uint8_t ddm[10] = { 0x23, 0x80, 0x80, 0xe8, 0x0c, 0x1c, 0x13, 0x94, 0x09, 0xd0 };
			memcpy(r, ddm, 10);	/* 35.5 degC, 3.3 V, 6.2 mA, 501.2 uW, 251.2 uW */
		}
		out = p[2];
		break;
	case REQ_TBL_READ: {
		if (old_bridge)
			return ST_UNKNOWN;
		uint32_t ctrl = get32(p);
		if (n != 5 || (ctrl & 3) != 1 || !p[4] || p[4] > TBL_READ_MAX)
			return ST_BAD_ARGUMENT;
		for (int i = 0; i < p[4]; i++) {
			tbl_ctrl(ctrl + ((uint32_t)i << 16));
			put32(r + out, regs[0x5cac]);
			put32(r + out + 4, regs[0x5ccc]);
			put32(r + out + 8, regs[0x5cd0]);
			put32(r + out + 12, regs[0x5cd4]);
			put32(r + out + 16, regs[0x5cb0]);
			out += 20;
		}
		break;
	}
	case REQ_STAT_READ: {
		if (old_bridge)
			return ST_UNKNOWN;
		uint16_t pmask = p[0] | p[1] << 8;
		if (n != 3u + p[2] || !p[2] || p[2] > STAT_READ_MAX)
			return ST_BAD_ARGUMENT;
		stat_reads++;
		for (int port = 0; port < 16; port++) {
			if (!(pmask & (1 << port)))
				continue;
			for (int i = 0; i < p[2]; i++) {
				uint64_t x = stat_value(port, p[3 + i]);
				put32(r + out, x);
				put32(r + out + 4, x >> 32);
				out += 8;
			}
		}
		break;
	}
	case REQ_SDS_CONFIG:
		if (old_bridge)
			return ST_UNKNOWN;
		sds_configs++;
		memcpy(sds_last, p, 2);
		break;
	case REQ_FLASH_ID:
		if (old_bridge)
			return ST_UNKNOWN;
		out = sprintf((char *)r, "Flash information:\n  Manufacturer ID: 0x%s\n", p[0] == 'j' ? "ef" : "--");
		break;
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
uint32_t plat_ms(void) { return uptime * 1000; }

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

static bool https_on, snmp_on;
static char snmp_comm[33] = "public";
static int newcerts;
bool plat_https_enable(bool on) { https_on = on; return true; }
bool plat_https_enabled(void) { return https_on; }
bool plat_https_newcert(void) { newcerts++; return true; }
void plat_https_status(char *fp, size_t len) { snprintf(fp, len, "AA:BB"); }
bool plat_snmp_set(int what, const char *v)
{
	if (what == PLAT_SNMP_ENABLED)
		snmp_on = v != NULL;
	else if (what == PLAT_SNMP_COMMUNITY) {
		if (strlen(v) >= sizeof(snmp_comm))
			return false;
		strcpy(snmp_comm, v);
	}
	return true;
}
const char *plat_snmp_get(int what)
{
	if (what == PLAT_SNMP_ENABLED)
		return snmp_on ? "1" : NULL;
	return what == PLAT_SNMP_COMMUNITY ? snmp_comm : "";
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

	/* VLAN table walks: TBL_READ in blocks of 64, or entry by entry from an older bridge */
	cmd("vlan 10 3 4t");
	cmd("vlan 4000 5");
	uint32_t r0 = requests;
	rtl_http_get("/vlanlist");
	uint32_t fast = requests - r0;
	static char fast_body[4096];
	memcpy(fast_body, outbuf, slen);
	fast_body[slen] = 0;
	check(strstr(fast_body, "\"id\":10,") && strstr(fast_body, "\"id\":4000,"),
	      "/vlanlist lists VLANs 10 and 4000");
	check(fast < 100, "/vlanlist in %u bridge requests", fast);
	cmd("vlan 20 6");	/* a write between two walks must not leave a stale prefetch */
	rtl_http_get("/vlanlist");
	outbuf[slen] = 0;
	check(strstr((char *)outbuf, "\"id\":20,") != NULL, "a VLAN added since the last walk shows up");
	old_bridge = true;
	cmd("vlan 20 d");
	r0 = requests;
	rtl_http_get("/vlanlist");
	outbuf[slen] = 0;
	check(!strcmp((char *)outbuf, fast_body), "an older bridge gives the same list, in %u requests", requests - r0);
	old_bridge = false;

	cmd("flash j");
	check(strstr((char *)outbuf, "Manufacturer ID: 0xef") != NULL, "flash j: the bridge's JEDEC dump");
	/* A module in slot 1, then a forced speed: the SerDes goes to 10GBase-R */
	sfp_module = true;
	rtl_on_sfp(0x30);
	ticks_run(200 * 3);
	check(sds_configs == 0, "a module at auto speed leaves the SerDes to the 8051");
	cmd("sfp 1 10g");
	check(sds_configs == 1 && sds_last[1] == 0x1a, "sfp 1 10g: SDS_CONFIG mode %02x", sds_last[1]);
	cmd("sfp 1 auto");
	check(sds_configs == 1, "sfp 1 auto sends nothing");

	/* The mod chip's own commands, through the parser's hook */
	cmd("https on");
	check(https_on && strstr((char *)outbuf, "200 OK"), "https on");
	cmd("https newcert");
	check(newcerts == 1, "https newcert");
	cmd("https");
	check(strstr((char *)outbuf, "HTTPS on, certificate SHA-256 AA:BB") != NULL, "https status");
	cmd("snmp community s3cret");
	cmd("snmp on");
	check(snmp_on && !strcmp(snmp_comm, "s3cret"), "snmp on, community");
	cmd("snmp community 0123456789012345678901234567890123456789");
	check(!memcmp(outbuf, "HTTP/1.1 400", 12), "an overlong community is refused");
	cmd("https sideways");
	check(!memcmp(outbuf, "HTTP/1.1 400", 12), "a bad https argument is refused");
	rtl_http_get("/cmd_log");
	outbuf[slen] = 0;
	check(strstr((char *)outbuf, "snmp community s3cret") && strstr((char *)outbuf, "https on"),
	      "the commands land in the history, so the saved config keeps them");

	/* The ports as the SNMP agent reads them */
	struct rtl_port port;
	check(rtl_port_count() == 6 && rtl_port_read(1, &port) && !strcmp(port.name, "Port 1") && !rtl_port_read(7, &port),
	      "port 1 of %u: name", rtl_port_count());

	/* All ports' counters in one STAT_READ; an older bridge gives the same, counter by counter */
	struct rtl_port pfast[RTL_MAX_PORTS], pslow[RTL_MAX_PORTS];
	uint32_t r1 = requests;
	int sr = stat_reads;
	uint8_t np = rtl_ports_read(pfast, RTL_MAX_PORTS);
	uint32_t fast_req = requests - r1;
	extern __code const struct machine machine;
	uint8_t lp = machine.phys_to_log_port[0];
	check(np == 6 && stat_reads == sr + 1 && pfast[0].in_octets == stat_value(lp, 0) && pfast[0].out_bcast == stat_value(lp, 7)
	      && pfast[0].out_discards == (uint32_t)(stat_value(lp, 8) >> 32) && pfast[0].in_discards == (uint32_t)stat_value(lp, 8)
	      && pfast[0].in_errors == (uint32_t)(stat_value(lp, 48) >> 32), "rtl_ports_read: counters from STAT_READ");
	old_bridge = true;
	r1 = requests;
	rtl_ports_read(pslow, RTL_MAX_PORTS);
	old_bridge = false;
	check(!memcmp(pfast, pslow, sizeof(pfast[0]) * np), "an older bridge gives the same counters");
	printf("COST ports+counters %u requests, %u without STAT_READ\n", fast_req, requests - r1);
	check(fast_req < 15, "all ports and counters in %u requests", fast_req);
	check(pfast[0].max_mbps == 2500 && pfast[3].max_mbps == 2500 && pfast[4].max_mbps == 10000 && pfast[5].max_mbps == 10000 && pfast[5].sfp,
	      "top speeds: ports 1-4 copper 2.5G, 5-6 SFP+ 10G");

	/* Link events from LINK messages: port 3's logical port comes up at 1G */
	uint8_t l3 = machine.phys_to_log_port[2];
	uint8_t lk[12] = { 0 };
	uint32_t sts = l3 < 8 ? 1u << (16 + l3) : 1u << l3, links = (uint32_t)2 << (4 * (l3 & 7));
	memcpy(lk, &sts, 4);
	memcpy(lk + (l3 < 8 ? 4 : 8), &links, 4);
	rtl_on_link(lk);
	memset(lk, 0, sizeof(lk));
	rtl_on_link(lk);
	struct rtl_link_event evs[8];
	uint8_t ne = rtl_link_events(evs, 8);
	check(ne == 2 && evs[0].port == 3 && evs[0].link == 3 && evs[1].port == 3 && evs[1].link == 0 && evs[1].seq > evs[0].seq,
	      "link events: port 3 up at 1G, then down");

	/* IGMP membership from the reports the CPU port sees */
	cmd("igmp on");
	uint8_t ig[64] = { 0x01, 0x00, 0x5e, 0x01, 0x02, 0x03, 2, 2, 2, 2, 2, 2,
			   0x88, 0x99, 0x04, 0x00, 0x00, 0x00, 0x00, 0x02,	/* CPU tag, port 2 */
			   0x08, 0x00,						/* IPv4, no 802.1Q tag */
			   0x46, 0, 0, 32, 0, 0, 0, 0, 1, 2, 0, 0, 10, 0, 0, 9, 239, 1, 2, 3, 0x94, 4, 0, 0,	/* router alert */
			   0x16, 0, 0, 0, 239, 1, 2, 3 };
	uint16_t vl;
	struct rtl_status st;
	rtl_rx_frame(ig, sizeof(ig), &vl);
	rtl_status_read(&st);
	check(st.igmp_on && st.igmp_groups == 1 && st.igmp_joins == 1, "IGMP: a v2 report joins a group");
	ig[46] = 0x17;
	rtl_rx_frame(ig, sizeof(ig), &vl);
	rtl_status_read(&st);
	check(st.igmp_groups == 0 && st.igmp_leaves == 1, "IGMP: a leave ends it");
	uint8_t v3[80];
	memcpy(v3, ig, 46);
	uint8_t rec[] = { 0x22, 0, 0, 0, 0, 0, 0, 2,	/* v3 report, 2 records */
			  4, 0, 0, 0, 239, 9, 9, 9,	/* TO_EXCLUDE {}: join */
			  2, 0, 0, 0, 239, 8, 8, 8 };	/* IS_EXCLUDE {}: join */
	memcpy(v3 + 46, rec, sizeof(rec));
	rtl_rx_frame(v3, 46 + sizeof(rec), &vl);
	rtl_status_read(&st);
	check(st.igmp_groups == 2, "IGMP: a v3 report with two records");

	/* STP root port and LACP partner changes, counted once a second */
	extern uint8_t stp_root_port;
	extern uint16_t lacp_partner_key[10];
	extern uint8_t lacpEnabled;
	uint32_t root0 = st.stp_root_changes, lacp0 = st.lacp_changes;
	stp_root_port = 2;
	lacpEnabled = 1;
	ticks_run(200);
	lacp_partner_key[1] = 0x1234;
	ticks_run(200);
	lacpEnabled = 0;
	rtl_status_read(&st);
	check(st.stp_root_changes > root0 && st.lacp_changes > lacp0, "STP root port and LACP partner changes counted");
	check((st.chip_temp == (int16_t)regs[0x0340]) && st.mac_entries == 0, "chip temperature and MAC entries read");

	/* SFP diagnostics from the module in slot 1 (inserted above) */
	struct rtl_sfp sf;
	rtl_sfp_read(0, &sf);
	check(sf.present && sf.ddm && sf.temp == 35 * 256 + 128 && sf.vcc == 33000 && sf.bias == 3100
	      && sf.tx_power == 5012 && sf.rx_power == 2512, "SFP diagnostics read");
	rtl_sfp_read(1, &sf);
	check(!sf.present, "SFP slot 2 empty");

	/* T3: "t3 test" before anything moves over */
	t3_mode = T3_DEAD;
	cmd("t3 test");
	check(strstr((char *)outbuf, "no answer on T3") != NULL, "t3 test: no answer on the bus");
	t3_mode = T3_DROPS_WRITES;
	regs[0x06f4] = 0x1234;
	cmd("t3 test");
	check(strstr((char *)outbuf, "PASS  chip ID matches") && strstr((char *)outbuf, "FAIL  written over T3")
	      && strstr((char *)outbuf, "keep register access on the bridge") && regs[0x06f4] == 0x1234,
	      "t3 test: writes without effect are caught, the test register restored");
	t3_mode = T3_GOOD;
	cmd("t3 test");
	check(strstr((char *)outbuf, "PASS  written over T3, read back over the bridge")
	      && strstr((char *)outbuf, "PASS  written over the bridge, read back over T3") && strstr((char *)outbuf, "T3 works")
	      && regs[0x06f4] == 0x1234, "t3 test: reads and writes both ways");
	cmd("t3");
	check(strstr((char *)outbuf, "T3 off") != NULL, "the test leaves register access on the bridge");

	/* "t3 on": plain register access over T3, batched reads still over the bridge */
	cmd("t3 on 400");
	uint32_t b0 = bridge_reg_ops, t0 = t3_ops;
	rtl_http_get("/status.json");
	check(bridge_reg_ops == b0 && t3_ops > t0, "t3 on: /status.json's registers go over T3, none over the bridge");
	int sr0 = stat_reads;
	struct rtl_port ptmp[RTL_MAX_PORTS];
	rtl_ports_read(ptmp, RTL_MAX_PORTS);
	/* (STAT_READ itself is off since the older-bridge test above: the
	 * firmware remembers a bridge without it until it restarts) */
	(void)sr0;
	check(ptmp[0].in_octets == stat_value(machine.phys_to_log_port[0], 0)
	      && ptmp[0].in_errors == (uint32_t)(stat_value(machine.phys_to_log_port[0], 48) >> 32),
	      "  counters read right with T3 on");
	r1 = requests;
	rtl_http_get("/vlanlist");
	outbuf[slen] = 0;
	check(strstr((char *)outbuf, "\"id\":10,") && requests - r1 < 100, "  /vlanlist still through TBL_READ");

	/* T3 failing: each access falls back to the bridge, then T3 is dropped */
	t3_mode = T3_FAILS;
	regs[0x0340] = 0x1680;	/* TM_RESULT: 45 degC */
	for (int i = 0; i < 4; i++)
		rtl_status_read(&st);
	check(st.chip_temp == 0x1680, "T3 failing: values still right, from the bridge");
	cmd("t3");
	check(strstr((char *)outbuf, "T3 off") && !strstr((char *)outbuf, "errors 0x00000000"),
	      "  and after 10 failures in a row, register access is back on the bridge");
	t3_mode = T3_GOOD;
	cmd("t3 off");

	/* Config writes through the flash API land in our sector */
	cmd("passwd secret");
	check(rtl_password_ok("secret"), "passwd changes the password");

	printf("REQUESTS %u\n", requests);
	printf("CONSOLE %zu\n", console_len);
	fwrite(console, 1, console_len, stdout);
	printf("\nDONE %d\n", failures);
	return failures ? 1 : 0;
}
