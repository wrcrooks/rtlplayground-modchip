#ifndef _T3_H_
#define _T3_H_

/* The T3 bus (t3.c) under the routing policy (t3_route.c, plat_t3_*()) */

#include <stdbool.h>
#include <stdint.h>

bool t3_bus_up(void);
bool t3_bus_read(uint16_t reg, uint32_t *v);
bool t3_bus_write(uint16_t reg, uint32_t v);
void t3_bus_speed(uint16_t khz);

#endif
