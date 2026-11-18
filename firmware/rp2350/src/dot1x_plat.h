#ifndef _DOT1X_PLAT_H_
#define _DOT1X_PLAT_H_

#include <stdbool.h>
#include <stdint.h>

/* dot1x_plat.c, called from rtl_app.c */
bool dot1x_plat_rx(const uint8_t *frame, uint16_t len, uint16_t type_at);
void dot1x_plat_links(uint32_t sts, uint32_t links, uint32_t links_89);
void dot1x_plat_bridge_up(void);
/* /dot1x.json */
void send_dot1x(void);

#endif
