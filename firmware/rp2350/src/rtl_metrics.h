#ifndef _RTL_METRICS_H_
#define _RTL_METRICS_H_

/* Hooks of rtl_metrics.c for rtl_app.c; what it offers others is in plat.h */

#include <stdint.h>

void rtl_metrics_link(uint32_t sts, uint32_t links, uint32_t links_89);
void rtl_metrics_second(void);
void rtl_metrics_igmp(const uint8_t *frame, uint16_t len);

#endif
