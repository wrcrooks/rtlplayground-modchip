/*
 * Hardware helpers for the mod-chip bridge firmware.
 *
 * The modules the bridge shares with RTLPlayground (chip init, PHY, SerDes,
 * SFP, LEDs, flash) call these helpers, which RTLPlayground defines in
 * rtlplayground.c next to its network stack. They are copied here unchanged
 * so that the bridge links without that stack. Keep them in step with
 * rtlplayground.c; modchip/bridge/check_hal.py compares the two.
 *
 * write_char() is the one helper that differs: the UART carries the bridge
 * protocol, so text goes out in LOG messages instead, see bridge.c.
 */

#include <8051.h>
#include <stdint.h>
#include <stdbool.h>

#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl837x_common.h"
#include "rtl837x_flash.h"
#include "rtl837x_pins.h"
#include "rtl837x_port.h"
#include "machine.h"
#include "phy.h"
#include "bridge.h"

extern __code const struct machine machine;
extern __xdata struct flash_region_t flash_region;
extern __xdata uint8_t flash_buf[FLASH_BUF_SIZE];
extern volatile __xdata uint16_t sleep_ticks;

__xdata uint8_t sfr_data[4];

__code const uint16_t bit_mask[16] = {
	0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080,
	0x0100, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000, 0x4000, 0x8000
};

/* Timer 2 runs the system tick, see SYS_TICK_HZ in bridge.h */
#define TIMER2_DIV (CLOCK_HZ / 12 / SYS_TICK_HZ)
#if TIMER2_DIV > 0xFFFF
#error "SYS_TICK_HZ to low, must be >= 159"
#endif
#define SYSTICK_TIMER2_VALUE (0x10000 - TIMER2_DIV)


void itoa(uint8_t v)
{
	uint8_t t = (v / 100);
	// when print_zeros is not zero, we know that a non-zero number has printed.
	// That have to print all the next numbers.
	uint8_t print_zeros = t;
	if (print_zeros)
		write_char('0' + t);
	t = (v / 10) % 10;
	print_zeros |= t;
	if (print_zeros)
		write_char('0' + t);
	write_char('0' + (v % 10));
}


/* Same as itoa(), one decade wider: enough for a port number. Kept separate
 * rather than widening itoa() itself, because every existing caller passes a
 * byte and would start paying for 16-bit divisions it does not need. */
void itoa_short(uint16_t v)
{
	uint8_t t = v / 10000;
	uint8_t print_zeros = t;

	if (print_zeros)
		write_char('0' + t);
	t = (v / 1000) % 10;
	print_zeros |= t;
	if (print_zeros)
		write_char('0' + t);
	t = (v / 100) % 10;
	print_zeros |= t;
	if (print_zeros)
		write_char('0' + t);
	t = (v / 10) % 10;
	print_zeros |= t;
	if (print_zeros)
		write_char('0' + t);
	write_char('0' + (v % 10));
}


void print_string(__code const char *p)
{
	while (*p)
		write_char(*p++);
}


void print_string_x(__xdata char *p)
{
	while (*p)
		write_char(*p++);
}


void print_short(uint16_t a)
{
	// allocating the registers first improves the sdcc code here
	uint8_t h = a >> 8;
	uint8_t l = a;

	print_string("0x");
	print_byte(h);
	print_byte(l);
}


void print_long(uint32_t a)
{
	// allocating the registers first improves the sdcc code here
	uint8_t a24 = a >> 24;
	uint8_t a16 = a >> 16;
	uint8_t a8 = a >> 8;
	uint8_t a0 = a;

	print_string("0x");
	print_byte(a24);
	print_byte(a16);
	print_byte(a8);
	print_byte(a0);
}


void print_byte(uint8_t a)
{
	char high = (a >> 4) + '0';
	if (high > '9') {
		high += 'a' - ('0' + 10);
	}
	write_char(high);

	char low = (a & 0xf) + '0';
	if (low > '9') {
		low += 'a' - ('0' + 10);
	}
	write_char(low);
}


void memcpy(__xdata void * __xdata dst, __xdata const void * __xdata src, uint16_t len)
{
	__xdata uint8_t *d = dst;
	__xdata const uint8_t *s = src;
	while (len--)
		*d++ = *s++;
}


int memcmp(__xdata const void *a, __xdata const void *b, uint16_t len)
{
	__xdata const uint8_t *x = a;
	__xdata const uint8_t *y = b;
	while (len--) {
		if (*x != *y)
			return *x - *y;
		x++;
		y++;
	}
	return 0;
}


void memcpyc(__xdata uint8_t *dst, __code const uint8_t *src, uint16_t len)
{
	while (len--)
		*dst++ = *src++;
}


void memset(__xdata uint8_t *dst, __xdata uint8_t v, uint8_t len)
{
	while (len--)
		*dst++ = v;
}


uint16_t strlen(__code const char *s)
{
	uint16_t l = 0;
	while (s[l])
		l++;
	return l;
}


uint16_t strlen_x(__xdata const char *s)
{
	uint16_t l = 0;
	while (s[l])
		l++;
	return l;
}


uint16_t strcpy(__xdata uint8_t *dst, const char *s)
{
	__xdata uint8_t *b = dst;
	while (*s)
		*dst++ = *s++;
	*dst = 0;
	return dst - b;
}


char strcmp(__xdata const uint8_t *a, __code const uint8_t *b)
{
	uint8_t i = 0;

	while (b[i] && (b[i] == a[i]))
		i++;

	if (a[i] < b[i])
		return -1;
	else if (a[i] > b[i])
		return 1;
	return 0;
}


void setup_timer2(void)
{
	T2CON = 0x00; // Timer2: Mode 16-bit timer with auto-reload, disable the timer.

	// Timer 2 clock select F_SYS / 12;
	// T2M = 0 uses clk/12;
	CKCON &= ~0x20;

	// The RCAP2 registers contain the high/low byte that is loaded into
	// timer2 when T2 overflows to 0x10000
	RCAP2_U16 = SYSTICK_TIMER2_VALUE;

	T2CON |= 0x04; // Timer2: Enable

	// IP |= 0x20; // TEST: Make Timer 2 interrupt as high priority.
	ET2 = 1; // Enable Timer2 interrupt.


}


void reg_read(uint16_t reg_addr)
{
	SFR_REG_ADDR_U16 = reg_addr;
	SFR_EXEC_GO = SFR_EXEC_READ_REG;
	do {
	} while (SFR_EXEC_STATUS != 0);
	/* The result is now in SFR A4, A5, A6, A7 */
}


void reg_read_m(uint16_t reg_addr)
{
#ifdef REGDBG
	if (EA) { write_char('r'); print_byte(reg_addr >> 8); print_byte(reg_addr); write_char(':'); }
#endif
	SFR_REG_ADDR_U16 = reg_addr;
	SFR_EXEC_GO = SFR_EXEC_READ_REG;
	do {
	} while (SFR_EXEC_STATUS != 0);
	sfr_data[0] = SFR_DATA_24;
	sfr_data[1] = SFR_DATA_16;
	sfr_data[2] = SFR_DATA_8;
	sfr_data[3] = SFR_DATA_0;
#ifdef REGDBG
	if (EA) { print_byte(sfr_data[0]);  print_byte(sfr_data[1]);  print_byte(sfr_data[2]);  print_byte(sfr_data[3]); write_char(' '); }
#endif
}


void reg_write(uint16_t reg_addr)
{
	/* Data to write must be in SFR A4, A5, A6, A7 */
	SFR_REG_ADDR_U16 = reg_addr;
	SFR_EXEC_GO = SFR_EXEC_WRITE_REG;
	do {
	} while (SFR_EXEC_STATUS != 0);
}


void reg_write_m(uint16_t reg_addr)
{
#ifdef REGDBG
	if (EA) {
		write_char('R'); print_byte(reg_addr >> 8); print_byte(reg_addr); write_char('-');
		print_byte(sfr_data[0]);  print_byte(sfr_data[1]);  print_byte(sfr_data[2]);  print_byte(sfr_data[3]); write_char(' ');
	}
#endif
	SFR_REG_ADDR_U16 = reg_addr;
	SFR_DATA_24 = sfr_data[0] ;
	SFR_DATA_16 = sfr_data[1];
	SFR_DATA_8 = sfr_data[2];
	SFR_DATA_0 = sfr_data[3];

	SFR_EXEC_GO = SFR_EXEC_WRITE_REG;
	do {
	} while (SFR_EXEC_STATUS != 0);
}


/*
 * This sets a bit in the 32bit wide switch register reg_addr
 */
void reg_bit_set(uint16_t reg_addr, char bit)
{
	uint8_t bit_mask = 1 << (bit & 0x7);

	bit >>= 3;
	reg_read_m(reg_addr);
	sfr_data[3-bit] |= bit_mask;
	reg_write_m(reg_addr);
}


/*
 * This sets a bit in the 32bit wide switch register reg_addr
 */
void reg_bit_clear(uint16_t reg_addr, char bit)
{
	uint8_t bit_mask = 1 << (bit & 0x7);

	bit >>= 3;
	reg_read_m(reg_addr);
	bit_mask = ~bit_mask;
	sfr_data[3-bit] &= bit_mask;
	reg_write_m(reg_addr);
}


/*
 * This tests a bit in the 32bit wide switch register reg_addr
 */
uint8_t reg_bit_test(uint16_t reg_addr, char bit)
{
	uint8_t bit_mask = 1 << (bit & 0x7);

	bit >>= 3;
	reg_read_m(reg_addr);
	bit_mask = bit_mask;
	if (sfr_data[3-bit] & bit_mask)
		return 1;
	return 0;
}


/*
 * This masks the sfr data fields, first &-ing with ~mask, then setting the bits in set
 */
void sfr_mask_data(uint8_t n, uint8_t mask, uint8_t set)
{
	uint8_t b = sfr_data[3-n];
	b &= ~mask;
	b |= set;
	sfr_data[3-n] = b;
}


/*
 * This zeros all the sfr data fields
 */
void sfr_set_zero(void) {
	uint8_t idx = 4;
	while (idx) {
		idx -= 1;
		sfr_data[idx] = 0;
	}
}


/*
 * Read a SerDes register in the SoC
 * Input must be: sds_id = 0/1, page < 128,  reg <= 0xff
 * The result is in SFR A6 and A7 (SFR_DATA_8, SFR_DATA_0)
 */
void sds_read(uint8_t sds_id, uint8_t page, uint8_t reg)
{
#ifdef REGDBG
	print_string("q"); print_byte(sds_id); print_byte(page); print_byte(reg);
#endif
	SFR_93 = reg;			// 93
	SFR_94 = page << 1 | sds_id;	// 94
	SFR_EXEC_GO = SFR_EXEC_READ_SDS;
	do {
	} while (SFR_EXEC_STATUS != 0);

#ifdef REGDBG
	write_char(':'); print_byte(SFR_DATA_8); print_byte(SFR_DATA_0); write_char(' ');
#endif
}


/*
 * Write a SerDes register in the SoC
 * Input must be: sds_id = 0/1, page < 128,  reg <= 0xff
 * The value written must be in SFR A6 and A7 (SFR_DATA_8, SFR_DATA_0)
 */
void sds_write_v(uint8_t sds_id, uint8_t page, uint8_t reg, uint16_t v)
{
#ifdef REGDBG
	print_string("Q"); print_byte(sds_id); print_byte(page); print_byte(reg);
	write_char(':'); print_byte(v >> 8); print_byte(v); write_char(' ');
#endif
	SFR_DATA_U16 = v;
	SFR_93 = reg;
	SFR_94 = page << 1 | sds_id;
	SFR_EXEC_GO = SFR_EXEC_WRITE_SDS;
	do {
	} while (SFR_EXEC_STATUS != 0);
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


void delay(uint16_t t)
{
	sleep_ticks = t;
	while (sleep_ticks > 0)
		PCON |= 1;
}


bool gpio_pin_test(uint8_t pin)
{
	reg_read_m(RTL837X_REG_GPIO_00_31_INPUT + (pin > 31 ? 4 : 0));
	return sfr_data[3-((pin >> 3) & 3)] & (1 << (pin & 7));
}


void flash_default_config(void)
{
	__xdata uint32_t source = DEFAULT_CONFIG_START;
	__xdata uint32_t dest = CONFIG_START;

	flash_region.addr = CONFIG_START;
	flash_sector_erase();

	for (uint8_t i = 0; i < 8; i++) // 8 * 512 Byte = 4 kByte (1 sector)
	{
		flash_region.addr = source;
		flash_region.len = FLASH_BUF_SIZE;
		flash_read_bulk(flash_buf);
		flash_region.addr = dest;
		flash_region.len = FLASH_BUF_SIZE;
		flash_write_bytes(flash_buf);
		dest += FLASH_BUF_SIZE;
		source += FLASH_BUF_SIZE;
	}

	print_string("Written default config to flash\n");
}


void reset_chip(void)
{
	REG_SET(RTL837X_REG_RESET, 1);
	while(1);
}


/*
 * Set dividers for a chosen CPU frequency
 */
void setup_clock(void)
{
	reg_read_m(RTL837X_REG_HW_CONF);
	sfr_mask_data(0, 0x30, 0);
#if CLOCK_DIV != 0
	 // Divider in bits 4 & 5
	sfr_mask_data(0, 0, CLOCK_DIV << 4);
#endif
	// Bit 8 is set in managed mode 125MHz to use fast SPI mode
	sfr_mask_data(1, 0, 0x01);
	reg_write_m(RTL837X_REG_HW_CONF);

	// Enable serial interface, set bit 0
	reg_read_m(RTL837X_PIN_MUX_1);
	sfr_mask_data(0, 0x1, 0x1);
	reg_write_m(RTL837X_PIN_MUX_1);
}


/*
 * Write a register reg of multipule phys, using a mask to select them, in page page
 * Data to be written is in v
 */
void phy_write_mask(uint16_t phy_mask, uint8_t dev_id, uint16_t reg, uint16_t v)
{
#ifdef REGDBG
	print_string("P"); print_byte(phy_mask>>8); print_byte(phy_mask); print_byte(dev_id); write_char('.'); print_byte(reg>>8); print_byte(reg); write_char(':');
	print_byte(v>>8); print_byte(v); write_char(' ');
#endif
	SFR_DATA_U16 = v;			    // SFR_A6, SFR_A7
	SFR_SMI_PHYMASK = phy_mask;		// SFR_C5
	SFR_SMI_REG_U16 = reg;			// SFR_C2, SFR_C3
	SFR_SMI_DEV = (phy_mask >> 8) | dev_id  << 3 | 2; // SFR_C4: bit 2 can also be set for some option
	SFR_EXEC_GO = SFR_EXEC_WRITE_SMI;
	do {
	} while (SFR_EXEC_STATUS != 0);
}


/*
 * Write a register reg of phy, using a mask to select them, in page page
 * Data to be written is in v
 */
void phy_write(uint8_t phy_id, uint8_t dev_id, uint16_t reg, uint16_t v)
{
	uint16_t phy_mask =  bit_mask[phy_id];
#ifdef REGDBG
	print_string("P"); print_byte(phy_mask>>8); print_byte(phy_mask); print_byte(dev_id); write_char('.'); print_byte(reg>>8); print_byte(reg); write_char(':');
	print_byte(v>>8); print_byte(v); write_char(' ');
#endif
	SFR_DATA_U16 = v;			    // SFR_A6, SFR_A7
	SFR_SMI_PHYMASK = phy_mask;		// SFR_C5
	SFR_SMI_REG_U16 = reg;			// SFR_C2, SFR_C3
	SFR_SMI_DEV = (phy_mask >> 8) | dev_id  << 3 | 2; // SFR_C4: bit 2 can also be set for some option
	SFR_EXEC_GO = SFR_EXEC_WRITE_SMI;
	do {
	} while (SFR_EXEC_STATUS != 0);
}


/*
 * Read a phy register via MDIO clause 45
 * Input must be: phy_id < 64,  device_id < 32,  reg < 0x10000)
 * The result is in SFR A6 and A7 (SFR_DATA_8, SFR_DATA_0)
 */
void phy_read(uint8_t phy_id, uint8_t dev_id, uint16_t reg)
{
#ifdef REGDBG
	print_string("p"); print_byte(phy_id); print_byte(dev_id); write_char('.'); print_byte(reg>>8); print_byte(reg); write_char(':');
#endif
	SFR_SMI_REG_U16 = reg;		// c2, c2

	SFR_SMI_PHY = phy_id;		// a5
	SFR_SMI_DEV = dev_id << 3 | 2;	// c4

	SFR_EXEC_GO = SFR_EXEC_READ_SMI;
	do {
	} while (SFR_EXEC_STATUS != 0);
#ifdef REGDBG
	print_byte(SFR_DATA_8); print_byte(SFR_DATA_0); write_char(' ');
#endif
}


void phy_modify(uint8_t phy_id, uint8_t dev_id, uint16_t reg, uint16_t mask, uint16_t set)
{
	uint8_t smi_phy = dev_id << 3 | 2;

	// Read the data
	SFR_SMI_REG_U16 = reg;		// c2, c2
	SFR_SMI_PHY = phy_id;		// a5
	SFR_SMI_DEV = smi_phy;		// c4
	SFR_EXEC_GO = SFR_EXEC_READ_SMI;
	do {
	} while (SFR_EXEC_STATUS != 0);

	// Modify the reed data.
	// TODO: Check if we directly can modify SFR register directly.
	uint16_t data = SFR_DATA_U16 & ~(mask);
	data |= set;

	uint16_t phy_mask = bit_mask[phy_id];

	// Write it back
	SFR_SMI_REG_U16 = reg;
	SFR_DATA_U16 = data;
	SFR_SMI_PHYMASK = phy_mask;		// SFR_C5
	SFR_SMI_DEV = smi_phy | (phy_mask >> 8);
	SFR_EXEC_GO = SFR_EXEC_WRITE_SMI;
	do {
	} while (SFR_EXEC_STATUS != 0);
}


void set_sys_led_state(uint8_t state)
{
	reg_read_m(RTL837X_REG_LED_MODE);
	sfr_mask_data(2, 0x03, state);
	reg_write_m(RTL837X_REG_LED_MODE);
}
