#ifndef _METRICS_H_
#define _METRICS_H_

/*
 * The dashboard's data: a sample every METRICS_PERIOD_S seconds into an
 * hour of history in RAM, and the current values on request. No Pico SDK
 * here, so the host tests run it (test/test_metrics.c).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define METRICS_PERIOD_S	10
#define METRICS_HISTORY		360		/* one hour */

/* The mod chip itself, from main.c */
struct plat_sys {
	uint16_t load_pm;		/* main loop busy, per mille, over the last second */
	uint32_t heap_free, heap_total;
	int16_t temp;			/* RP2350, 0.1 degC */
	bool watchdog_reset;		/* this boot followed a watchdog timeout */
};
void plat_sys_read(struct plat_sys *s);

void metrics_second(void);		/* once a second */
size_t metrics_json(char *buf, size_t max);	/* /metrics.json, body only */
uint8_t *metrics_history(size_t *len);	/* /history.bin, malloc()ed; NULL: nothing yet */

#endif
