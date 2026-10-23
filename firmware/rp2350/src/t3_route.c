/*
 * When register access goes over T3 instead of the bridge: once "t3 on"
 * enables it, while the bus is up. A failed transfer falls back to the
 * bridge for that access; after T3_GIVE_UP failures in a row, register
 * access goes back to the bridge for good, until "t3 on" again. No
 * hardware here, so the simulation runs it (test/sim.c).
 */

#include <stdio.h>

#include "plat.h"
#include "t3.h"

#define T3_GIVE_UP		10

static bool enabled;
static uint16_t khz = 100;
static uint32_t reads, writes, errors;
static uint8_t in_a_row;


static bool counted(bool ok)
{
	if (ok) {
		in_a_row = 0;
		return true;
	}
	errors++;
	if (++in_a_row >= T3_GIVE_UP) {
		enabled = false;
		in_a_row = 0;
		printf("t3: %u failures in a row, register access goes over the bridge again\n", T3_GIVE_UP);
	}
	return false;
}


bool plat_t3_read(uint16_t reg, uint32_t *v)
{
	if (!enabled || !t3_bus_up())
		return false;
	reads++;
	return counted(t3_bus_read(reg, v));
}


bool plat_t3_write(uint16_t reg, uint32_t v)
{
	if (!enabled || !t3_bus_up())
		return false;
	writes++;
	return counted(t3_bus_write(reg, v));
}


bool plat_t3_raw_read(uint16_t reg, uint32_t *v)
{
	return t3_bus_up() && t3_bus_read(reg, v);
}


bool plat_t3_raw_write(uint16_t reg, uint32_t v)
{
	return t3_bus_up() && t3_bus_write(reg, v);
}


void plat_t3_enable(bool on, uint16_t speed)
{
	if (speed && speed != khz) {
		khz = speed;
		t3_bus_speed(khz);
	}
	enabled = on;
	in_a_row = 0;
}


bool plat_t3_enabled(void)
{
	return enabled && t3_bus_up();
}


void plat_t3_status(struct plat_t3 *s)
{
	s->up = t3_bus_up();
	s->enabled = enabled;
	s->khz = khz;
	s->reads = reads;
	s->writes = writes;
	s->errors = errors;
}
