/*
 * T3: the RTL8372's slave interface over I2C, address 0x5c, for register
 * access without the bridge.
 *
 *   read:   S 5c+W  reg_hi reg_lo  Sr 5c+R  v0 v1 v2 v3  P
 *   write:  S 5c+W  reg_hi reg_lo  v0 v1 v2 v3  P
 *
 * Register address high byte first, value low byte first. The write is
 * one plain transfer, as vDorst/rtl-rs does it against the same interface
 * (and its SFP, PHY and SerDes access through registers works only if
 * writes do); "t3 test" checks it on the switch at hand before register
 * access is moved over.
 *
 * The bus; when register access goes over it is t3_route.c's business.
 * The pins are configured only while the switch has power, as the bridge
 * UART's. Pull-ups: 4.7 kOhm to the switch's 3.3 V on the mod chip; on a
 * test rig, wire them to T3 pin 4, not to the stand-in board's own 3.3 V.
 */

#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/i2c.h"

#include "net.h"
#include "t3.h"

#define T3_ADDR			0x5c
#define T3_TIMEOUT_US		5000

static i2c_inst_t *const bus = I2C_INSTANCE(MODCHIP_T3_I2C);
static bool up;
static uint16_t khz = 100;


void t3_power(bool on)
{
	if (on == up)
		return;
	if (on) {
		i2c_init(bus, khz * 1000u);
		gpio_set_function(MODCHIP_T3_SDA_PIN, GPIO_FUNC_I2C);
		gpio_set_function(MODCHIP_T3_SCL_PIN, GPIO_FUNC_I2C);
		gpio_disable_pulls(MODCHIP_T3_SDA_PIN);	/* external pull-ups, to the switch's 3.3 V */
		gpio_disable_pulls(MODCHIP_T3_SCL_PIN);
	} else {
		i2c_deinit(bus);
		gpio_deinit(MODCHIP_T3_SDA_PIN);
		gpio_deinit(MODCHIP_T3_SCL_PIN);
		gpio_set_input_enabled(MODCHIP_T3_SDA_PIN, false);
		gpio_set_input_enabled(MODCHIP_T3_SCL_PIN, false);
	}
	up = on;
}


bool t3_bus_read(uint16_t reg, uint32_t *v)
{
	uint8_t a[2] = { reg >> 8, reg }, d[4];

	if (!up || i2c_write_timeout_us(bus, T3_ADDR, a, 2, true, T3_TIMEOUT_US) != 2
	    || i2c_read_timeout_us(bus, T3_ADDR, d, 4, false, T3_TIMEOUT_US) != 4)
		return false;
	*v = d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24;
	return true;
}


bool t3_bus_write(uint16_t reg, uint32_t v)
{
	uint8_t d[6] = { reg >> 8, reg, v, v >> 8, v >> 16, v >> 24 };

	return up && i2c_write_timeout_us(bus, T3_ADDR, d, 6, false, T3_TIMEOUT_US) == 6;
}


bool t3_bus_up(void)
{
	return up;
}


void t3_bus_speed(uint16_t speed)
{
	khz = speed;
	if (up)
		i2c_set_baudrate(bus, khz * 1000u);
}
