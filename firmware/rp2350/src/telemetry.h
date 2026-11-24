#ifndef _TELEMETRY_H_
#define _TELEMETRY_H_

/*
 * The switch's state for others: MQTT, to a broker in plain or over TLS, and
 * GET /metrics in Prometheus's text format (metrics.c, httpd.c).
 *
 * Topics, below "mqtt topic" (the hostname at first):
 *   status       "online", or "offline" as the broker's last will; retained
 *   switch       temperature, uptime and so on, each interval; retained
 *   port/<n>     link, counters, each interval; retained
 *   event        each event as it happens (events.h)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void telemetry_second(void);

#endif
