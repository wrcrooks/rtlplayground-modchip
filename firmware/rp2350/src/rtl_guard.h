#ifndef _RTL_GUARD_H_
#define _RTL_GUARD_H_

/*
 * Error-disable, DHCP snooping and QoS (rtl_guard.c).
 *
 * An error-disabled port is fenced off as a closed 802.1X port is: it
 * forwards nothing, learns nothing and its frames to the CPU are dropped,
 * while its link stays up. It comes back after the recovery time, or with
 * "errdisable port <n> clear".
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void rtl_guard_reset(void);		/* rtl_bridge_up(), before the config */
void rtl_guard_second(void);
void rtl_guard_link(uint8_t port, uint8_t link);	/* logical port, rtl_port.link */
uint16_t rtl_errdis(void);		/* logical ports */
bool rtl_errdis_limit(uint8_t port);	/* port's l2 limit shuts it down */
size_t rtl_guard_json(char *buf, size_t max);

/* rtl_env.c: the ports error-disable fences off */
void rtl_iso_errdis(uint16_t ports);
uint32_t rtl_l2_limit(uint8_t port);	/* as configured */

#endif
