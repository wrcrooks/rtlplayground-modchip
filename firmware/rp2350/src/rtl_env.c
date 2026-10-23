/*
 * What RTLPlayground's rtlplayground.c and httpd.c provide on the 8051, for
 * its portable modules compiled on the RP2350: the globals, the console
 * helpers and the chip access functions. The 8051 drives the register, SMI
 * and SerDes engines through SFRs; here each access becomes a bridge
 * request, and the results land in the same places (sfr_data, the SFR_DATA_*
 * bytes, see rtl_sfr.S) so the calling code reads them unchanged.
 *
 * Compiled like RTLPlayground's sources, with sdcc_shim.h forced in.
 */

#include <8051.h>
#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl837x_flash.h"
#include "rtl837x_leds.h"
#include "machine.h"
#include "dhcp.h"
#include "syslog.h"
#include "cmd_parser.h"
#include "uip/uip.h"
#include "httpd/page_impl.h"

#include "bridge.h"
#include "plat.h"

/* page_impl.h defines itohex() inline: one TU must also emit it */
extern char itohex(uint8_t val);

/* ---- globals of rtlplayground.c ---- */

extern __code const struct machine machine;

__xdata struct machine_runtime machine_detected;
__xdata struct uip_eth_addr uip_ethaddr;
__xdata uint8_t sfr_data[4];
__code const uint8_t * __code const hex = "0123456789abcdef";
__xdata uint8_t flash_buf[FLASH_BUF_SIZE];
__xdata uint8_t uip_buf[UIP_CONF_BUFFER_ALLOC_SIZE];
__xdata uint16_t management_vlan;
__xdata bool stp_enabled;
__xdata uint8_t igmpEnabled;
__xdata bool lldp_enabled;
__xdata char hostname[24];
__xdata uint8_t lacpEnabled;
__xdata volatile uint8_t sbuf_ptr;
__xdata uint8_t sbuf[SBUF_SIZE];
volatile __xdata uint32_t ticks;
__bit cmd_available;
__xdata uint8_t cmd_capture;
__xdata uint32_t rtl8224_release_tick;
__xdata uint16_t uip_len;
__xdata uip_ipaddr_t uip_hostaddr, uip_draddr, uip_netmask;
__xdata struct dhcp_state dhcp_state;

__code const uint16_t bit_mask[16] = {
	0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080,
	0x0100, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000, 0x4000, 0x8000
};

/* ---- globals of httpd.c that page_impl.c and cmd_parser.c use ---- */

__xdata uint8_t outbuf[TCP_OUTBUF_SIZE];
__xdata uint16_t slen;
__xdata uint16_t cont_len;
__xdata uint32_t cont_addr;
__xdata char passwd[21];
__xdata uint16_t session_timeout = 200;

/* ---- the console ---- */

void write_char_no_syslog(char c)
{
	if (cmd_capture) {
		if (slen < TCP_OUTBUF_SIZE - sizeof(CMD_TRUNCATED))
			outbuf[slen++] = c;
		else
			cmd_capture = 2;
	}
	if (c == '\n')
		plat_console_putc('\r');
	plat_console_putc(c);
}


void write_char(char c)
{
	write_char_no_syslog(c);

	if (syslog_state.enabled) {
		uint16_t next = (syslog_state.writeptr + 1) & (LOGBUF_SIZE - 1);

		if (next == syslog_state.readptr) {
			syslog_state.line_available = 1;
			return;
		}
		logbuf[syslog_state.writeptr] = c;
		syslog_state.writeptr = next;
		if (c == '\n')
			syslog_state.line_available = 1;
	}
}


void itoa(uint8_t v)
{
	if (v >= 100)
		write_char('0' + v / 100);
	if (v >= 10)
		write_char('0' + (v / 10) % 10);
	write_char('0' + v % 10);
}


void itoa_short(uint16_t v)
{
	char b[5];
	uint8_t n = 0;

	do {
		b[n++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (n)
		write_char(b[--n]);
}


void print_string(__code const char *p)
{
	while (*p)
		write_char(*p++);
}


void print_string_no_syslog(__code const char *p)
{
	while (*p)
		write_char_no_syslog(*p++);
}


void print_string_newline_no_syslog(__code const char *p)
{
	write_char_no_syslog('\n');
	print_string_no_syslog(p);
}


void print_string_x(__xdata char *p)
{
	while (*p)
		write_char(*p++);
}


void print_byte(uint8_t a)
{
	write_char(hex[a >> 4]);
	write_char(hex[a & 0xf]);
}


void print_short(uint16_t a)
{
	print_string("0x");
	print_byte(a >> 8);
	print_byte(a);
}


void print_long(uint32_t a)
{
	print_string("0x");
	print_byte(a >> 24);
	print_byte(a >> 16);
	print_byte(a >> 8);
	print_byte(a);
}


void print_cmd_prompt(void)
{
	print_string_no_syslog("\n> ");
}


void print_sfr_data(void)
{
	write_char('0');
	write_char('x');
	print_byte(sfr_data[0]);
	print_byte(sfr_data[1]);
	print_byte(sfr_data[2]);
	print_byte(sfr_data[3]);
}


void print_phy_data(void)
{
	write_char('0');
	write_char('x');
	print_byte(SFR_DATA_8);
	print_byte(SFR_DATA_0);
}


void print_reg(uint16_t reg)
{
	reg_read_m(reg);
	print_sfr_data();
}


void print_phys_port(uint8_t port)
{
	if (port < CPU_PORT)
		write_char(machine.log_to_phys_port[port] + '0');
	else if (port == CPU_PORT)
		print_string("CPU");
	else {
		print_string("UNKNOWN ");
		write_char(port + '0');
	}
}

/* ---- string helpers that shadow nothing in libc ---- */

void memcpyc(__xdata uint8_t *dst, __code const uint8_t *src, uint16_t len)
{
	memcpy(dst, src, len);
}


uint16_t strtox(__xdata uint8_t *dst, __code const char *s)
{
	__xdata uint8_t *b = dst;

	while (*s)
		*dst++ = *s++;
	*dst = 0;
	return dst - b;
}


uint16_t strlen_x(__xdata const char *s)
{
	return strlen(s);
}


bool strstart(__xdata const uint8_t *a, __code const uint8_t *b)
{
	while (*b && *b == *a) {
		a++;
		b++;
	}
	return !*b;
}


bool strstart_x(__xdata const uint8_t *a, __xdata const uint8_t *b)
{
	return strstart(a, b);
}

/* ---- chip access over the bridge ---- */

static uint32_t get32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}


static void sfr_put32(uint32_t v)
{
	SFR_DATA_0 = v;
	SFR_DATA_8 = v >> 8;
	SFR_DATA_16 = v >> 16;
	SFR_DATA_24 = v >> 24;
}


/*
 * A failed request leaves zeros: polling loops in the modules wait for busy
 * bits to clear, so zeros end them rather than hanging the firmware.
 */
/*
 * The table engine. Reading an entry takes three steps - write the command
 * to TBL_CTRL, poll TBL_CTRL until done, read the results - and each step
 * would be a bridge round trip. A read command instead goes out as one
 * TBL_READ, and the poll and the result registers are answered from its
 * reply until the next register write. VLAN entries are read 64 at a time,
 * for the walks over all 4094 of them (send_vlanlist()).
 */
#define TBL_SHADOW_REGS		5
#define TBL_PREFETCH_TICKS	(SYS_TICK_HZ / 5)

static __code const uint16_t tbl_regs[TBL_SHADOW_REGS] = {
	RTL837X_TBL_CTRL, RTL837x_L2_DATA_OUT_A, RTL837x_L2_DATA_OUT_B, RTL837x_L2_DATA_OUT_C, RTL837x_TBL_DATA_0
};
static bool tbl_read_unsupported;	/* a bridge from before TBL_READ */
static bool shadow_valid;
static uint32_t shadow[TBL_SHADOW_REGS];
static struct {
	uint16_t cmd;			/* TBL_CTRL's low half: table type and command */
	uint16_t first;
	uint8_t n;
	uint32_t at;			/* ticks */
	uint32_t v[TBL_READ_MAX][TBL_SHADOW_REGS];
} pre;

static void tbl_invalidate(void)
{
	shadow_valid = false;
	pre.n = 0;
}


static void shadow_from(const uint32_t *v)
{
	memcpy(shadow, v, sizeof(shadow));
	shadow[0] &= ~(uint32_t)TBL_EXECUTE;	/* polls end even if the bridge's timed out */
	shadow_valid = true;
}


/* False: do the write the ordinary way */
static bool tbl_read(uint32_t ctrl)
{
	static uint8_t r[TBL_READ_MAX * TBL_SHADOW_REGS * 4];
	uint16_t entry = ctrl >> 16;
	uint8_t count = 1;
	size_t n;

	if (tbl_read_unsupported)
		return false;
	if (((ctrl >> 8) & 0xff) == TBL_VLAN) {
		if (pre.n && pre.cmd == (uint16_t)ctrl && entry >= pre.first && entry < pre.first + pre.n
		    && ticks - pre.at < TBL_PREFETCH_TICKS) {
			shadow_from(pre.v[entry - pre.first]);
			return true;
		}
		count = entry < 0xfff ? (0xfff - entry > TBL_READ_MAX ? TBL_READ_MAX : 0xfff - entry) : 1;
	}
	uint8_t p[5] = { ctrl, ctrl >> 8, ctrl >> 16, ctrl >> 24, count };
	int st = bridge_request(REQ_TBL_READ, p, 5, NULL, 0, r, sizeof(r), &n);
	if (st == ST_UNKNOWN) {
		tbl_read_unsupported = true;
		return false;
	}
	if (st != ST_OK || n != (size_t)count * TBL_SHADOW_REGS * 4) {
		/* Zeros, as for a failed read: the poll ends, the entry reads empty */
		memset(shadow, 0, sizeof(shadow));
		shadow_valid = true;
		return true;
	}
	pre.n = 0;
	for (uint8_t i = 0; i < count; i++)
		for (uint8_t k = 0; k < TBL_SHADOW_REGS; k++)
			pre.v[i][k] = get32(r + (i * TBL_SHADOW_REGS + k) * 4);
	shadow_from(pre.v[0]);
	if (count > 1) {
		pre.cmd = ctrl;
		pre.first = entry;
		pre.n = count;
		pre.at = ticks;
	}
	return true;
}


void reg_read(uint16_t reg_addr)
{
	uint8_t p[2] = { reg_addr, reg_addr >> 8 };
	uint8_t r[4];
	size_t n;
	uint32_t v = 0;

	if (shadow_valid) {
		for (uint8_t k = 0; k < TBL_SHADOW_REGS; k++) {
			if (tbl_regs[k] == reg_addr) {
				sfr_put32(shadow[k]);
				return;
			}
		}
	}
	/* The bridge leaves the seconds counter alone; RTLPlayground's 8051
	 * firmware advanced it in its tick, so here it reads our uptime. */
	if (reg_addr == RTL837X_REG_SEC_COUNTER)
		v = plat_uptime_s();
	else if (plat_t3_read(reg_addr, &v))
		;	/* over T3, when enabled ("t3 on") */
	else if (bridge_request(REQ_REG_READ, p, 2, NULL, 0, r, 4, &n) == ST_OK && n == 4)
		v = get32(r);
	sfr_put32(v);
}


/* The bridge's REG_READ/REG_WRITE, whatever the routing: for "t3 test" */
bool rtl_bridge_reg_read(uint16_t reg, uint32_t *v)
{
	uint8_t p[2] = { reg, reg >> 8 }, r[4];
	size_t n;

	if (bridge_request(REQ_REG_READ, p, 2, NULL, 0, r, 4, &n) != ST_OK || n != 4)
		return false;
	*v = get32(r);
	return true;
}


bool rtl_bridge_reg_write(uint16_t reg, uint32_t v)
{
	uint8_t p[6] = { reg, reg >> 8, v, v >> 8, v >> 16, v >> 24 };

	return bridge_request(REQ_REG_WRITE, p, 6, NULL, 0, NULL, 0, NULL) == ST_OK;
}


void reg_read_m(uint16_t reg_addr)
{
	reg_read(reg_addr);
	sfr_data[0] = SFR_DATA_24;
	sfr_data[1] = SFR_DATA_16;
	sfr_data[2] = SFR_DATA_8;
	sfr_data[3] = SFR_DATA_0;
}


void reg_write(uint16_t reg_addr)
{
	uint8_t p[6] = { reg_addr, reg_addr >> 8, SFR_DATA_0, SFR_DATA_8, SFR_DATA_16, SFR_DATA_24 };

	if (reg_addr == RTL837X_REG_SEC_COUNTER)
		return;
	if (reg_addr == RTL837X_TBL_CTRL && (p[2] & (TBL_WRITE | TBL_EXECUTE)) == TBL_EXECUTE
	    && tbl_read(get32(p + 2)))
		return;
	/* Any write may change what a table read returns */
	tbl_invalidate();
	if (!plat_t3_write(reg_addr, get32(p + 2)))
		bridge_request(REQ_REG_WRITE, p, 6, NULL, 0, NULL, 0, NULL);
}


void reg_write_m(uint16_t reg_addr)
{
	SFR_DATA_24 = sfr_data[0];
	SFR_DATA_16 = sfr_data[1];
	SFR_DATA_8 = sfr_data[2];
	SFR_DATA_0 = sfr_data[3];
	reg_write(reg_addr);
}


void reg_bit_set(uint16_t reg_addr, char bit)
{
	uint8_t m = 1 << (bit & 0x7);

	bit >>= 3;
	reg_read_m(reg_addr);
	sfr_data[3 - bit] |= m;
	reg_write_m(reg_addr);
}


void reg_bit_clear(uint16_t reg_addr, char bit)
{
	uint8_t m = 1 << (bit & 0x7);

	bit >>= 3;
	reg_read_m(reg_addr);
	sfr_data[3 - bit] &= ~m;
	reg_write_m(reg_addr);
}


uint8_t reg_bit_test(uint16_t reg_addr, char bit)
{
	uint8_t m = 1 << (bit & 0x7);

	bit >>= 3;
	reg_read_m(reg_addr);
	return (sfr_data[3 - bit] & m) ? 1 : 0;
}


void sfr_mask_data(uint8_t n, uint8_t mask, uint8_t set)
{
	sfr_data[3 - n] = (sfr_data[3 - n] & ~mask) | set;
}


void sfr_set_zero(void)
{
	sfr_data[0] = sfr_data[1] = sfr_data[2] = sfr_data[3] = 0;
}


void get_random_32(void)
{
	uint32_t v = plat_random32();

	sfr_data[0] = v >> 24;
	sfr_data[1] = v >> 16;
	sfr_data[2] = v >> 8;
	sfr_data[3] = v;
}


void read_reg_timer(__xdata uint32_t *tmr)
{
	*tmr = plat_uptime_s();
}


void phy_read(uint8_t phy_id, uint8_t dev_id, uint16_t reg)
{
	uint8_t p[4] = { phy_id, dev_id, reg, reg >> 8 };
	uint8_t r[2];
	size_t n;

	if (bridge_request(REQ_PHY_READ, p, 4, NULL, 0, r, 2, &n) == ST_OK && n == 2)
		SFR_DATA_U16 = r[0] | r[1] << 8;
	else
		SFR_DATA_U16 = 0;
}


void phy_write(uint8_t phy_id, uint8_t dev_id, uint16_t reg, uint16_t v)
{
	uint8_t p[6] = { phy_id, dev_id, reg, reg >> 8, v, v >> 8 };

	SFR_DATA_U16 = v;
	bridge_request(REQ_PHY_WRITE, p, 6, NULL, 0, NULL, 0, NULL);
}


/* The SMI engine writes all PHYs in the mask at once; the bridge takes one */
void phy_write_mask(uint16_t phy_mask, uint8_t dev_id, uint16_t reg, uint16_t v)
{
	for (uint8_t i = 0; i < 16; i++)
		if (phy_mask & bit_mask[i])
			phy_write(i, dev_id, reg, v);
}


void phy_modify(uint8_t phy_id, uint8_t dev_id, uint16_t reg, uint16_t mask, uint16_t set)
{
	phy_read(phy_id, dev_id, reg);
	phy_write(phy_id, dev_id, reg, (SFR_DATA_U16 & ~mask) | set);
}


void sds_read(uint8_t sds_id, uint8_t page, uint8_t reg)
{
	uint8_t p[3] = { sds_id, page, reg };
	uint8_t r[2];
	size_t n;

	if (bridge_request(REQ_SDS_READ, p, 3, NULL, 0, r, 2, &n) == ST_OK && n == 2)
		SFR_DATA_U16 = r[0] | r[1] << 8;
	else
		SFR_DATA_U16 = 0;
}


void sds_write_v(uint8_t sds_id, uint8_t page, uint8_t reg, uint16_t v)
{
	uint8_t p[5] = { sds_id, page, reg, v, v >> 8 };

	SFR_DATA_U16 = v;
	bridge_request(REQ_SDS_WRITE, p, 5, NULL, 0, NULL, 0, NULL);
}


/* The 8051 owns the SerDes and runs sds_config() itself */
void sds_config(uint8_t sds, uint8_t mode)
{
	uint8_t p[2] = { sds, mode };

	if (bridge_request(REQ_SDS_CONFIG, p, 2, NULL, 0, NULL, 0, NULL) != ST_OK)
		print_string("SerDes configuration failed\n");
}


/* sfp_read_block() in rtl837x_pins.c drives the SoC's I2C master, which the
 * 8051 owns; the link wraps calls to it here (-Wl,--wrap=sfp_read_block). */
extern __xdata uint8_t sfp_buf[16];

bool __wrap_sfp_read_block(uint8_t slot, uint8_t reg, uint8_t len)
{
	uint8_t p[3] = { slot, reg, len };
	size_t n;

	if (len < 1 || len > 16)
		return false;
	return bridge_request(REQ_SFP_READ, p, 3, NULL, 0, sfp_buf, sizeof(sfp_buf), &n) == ST_OK && n == len;
}


bool gpio_pin_test(uint8_t pin)
{
	reg_read_m(RTL837X_REG_GPIO_00_31_INPUT + (pin > 31 ? 4 : 0));
	return sfr_data[3 - ((pin >> 3) & 3)] & (1 << (pin & 7));
}


void set_sys_led_state(uint8_t state)
{
	plat_sys_led(state);
}


void delay(uint16_t t)
{
	plat_delay_ms((uint32_t)t * (1000 / SYS_TICK_HZ));
}


/* The 8051 ran its main loop while sleeping; nothing here relies on that */
unsigned int sleep(unsigned int t)
{
	plat_delay_ms((uint32_t)t * (1000 / SYS_TICK_HZ));
	return 0;
}


void reset_chip(void)
{
	plat_reboot();
}

/*
 * ---- flash ----
 *
 * The modules address one flash. The saved configuration, CONFIG_START to
 * CONFIG_START + CONFIG_LEN, maps to a sector of the RP2350's own flash;
 * every other address is the switch's flash, through the bridge, which
 * only allows writes above the running image.
 */

__xdata struct flash_region_t flash_region;

static bool is_config(uint32_t addr)
{
	return addr >= CONFIG_START && addr < CONFIG_START + CONFIG_LEN;
}


void rtl_config_read(uint32_t off, uint8_t *dst, uint16_t len)
{
	if (off >= CONFIG_LEN) {
		memset(dst, 0xff, len);
		return;
	}
	if (off + len > CONFIG_LEN) {
		memset(dst + (CONFIG_LEN - off), 0xff, off + len - CONFIG_LEN);
		len = CONFIG_LEN - off;
	}
	plat_config_read(off, dst, len);
}


void flash_read_bulk(__xdata uint8_t *dst)
{
	uint32_t addr = flash_region.addr;
	uint16_t len = flash_region.len;

	if (is_config(addr)) {
		rtl_config_read(addr - CONFIG_START, dst, len);
		return;
	}
	while (len) {
		uint16_t n = len > 512 ? 512 : len;
		uint8_t p[6] = { addr, addr >> 8, addr >> 16, addr >> 24, n, n >> 8 };
		size_t got;

		if (bridge_request(REQ_FLASH_READ, p, 6, NULL, 0, dst, n, &got) != ST_OK || got != n)
			memset(dst, 0xff, n);
		addr += n;
		dst += n;
		len -= n;
	}
}


/* A config sector rewrite collects here and goes to flash on the last write */
static __xdata uint8_t config_shadow[CONFIG_LEN];

void flash_sector_erase(void)
{
	uint32_t addr = flash_region.addr;
	uint8_t p[4] = { addr, addr >> 8, addr >> 16, addr >> 24 };

	if (is_config(addr)) {
		memset(config_shadow, 0xff, sizeof(config_shadow));
		plat_config_write(config_shadow, CONFIG_LEN);
		return;
	}
	bridge_request(REQ_FLASH_ERASE, p, 4, NULL, 0, NULL, 0, NULL);
}


void flash_write_bytes(__xdata uint8_t *src)
{
	uint32_t addr = flash_region.addr;
	uint16_t len = flash_region.len;

	if (is_config(addr)) {
		uint32_t off = addr - CONFIG_START;

		if (off + len > CONFIG_LEN)
			len = CONFIG_LEN - off;
		plat_config_read(0, config_shadow, CONFIG_LEN);
		for (uint16_t i = 0; i < len; i++)
			config_shadow[off + i] &= src[i];	/* as NOR flash programs */
		plat_config_write(config_shadow, CONFIG_LEN);
		return;
	}
	while (len) {
		uint16_t n = 256 - (addr & 0xff);
		if (n > len)
			n = len;
		uint8_t p[4] = { addr, addr >> 8, addr >> 16, addr >> 24 };
		bridge_request(REQ_FLASH_WRITE, p, 4, src, n, NULL, 0, NULL);
		addr += n;
		src += n;
		len -= n;
	}
}


/* The flash's ID registers are the 8051's to read: it runs the same
 * functions of rtl837x_flash.c and returns what they print */
static void flash_id(char what, uint32_t addr)
{
	uint8_t p[5] = { what, addr, addr >> 8, addr >> 16, addr >> 24 };
	char text[129];
	size_t n;

	if (bridge_request(REQ_FLASH_ID, p, 5, NULL, 0, text, sizeof(text) - 1, &n) != ST_OK) {
		print_string("The bridge doesn't answer FLASH_ID\n");
		return;
	}
	text[n] = 0;
	print_string_x(text);
}


void flash_read_jedecid(void)
{
	flash_id('j', 0);
}


void flash_read_uid(void)
{
	flash_id('u', 0);
}


void flash_read_security(void)
{
	flash_id('s', flash_region.addr);
	flash_region.addr += flash_region.len;
}


__code const char *get_flash_size_str(void)
{
	switch (bridge_hello.flash_size) {
	case 0x40000: return "256 KB";
	case 0x80000: return "512 KB";
	case 0x100000: return "1 MB";
	case 0x200000: return "2 MB";
	case 0x400000: return "4 MB";
	case 0x800000: return "8 MB";
	case 0x1000000: return "16 MB";
	default: return "unknown";
	}
}


/* The default configuration, RTLPlayground's config.txt, embedded by CMake */
extern const char rtl_default_config[];

void flash_default_config(void)
{
	uint32_t n = strlen(rtl_default_config) + 1;

	memset(config_shadow, 0xff, sizeof(config_shadow));
	memcpy(config_shadow, rtl_default_config, n > CONFIG_LEN ? CONFIG_LEN : n);
	plat_config_write(config_shadow, CONFIG_LEN);
	print_string("Written default config to flash\n");
}
