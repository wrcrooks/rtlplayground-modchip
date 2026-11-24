#ifndef _EVENTS_H_
#define _EVENTS_H_

/*
 * Things worth telling somebody: link changes, logins, 802.1X, error-disable,
 * configuration and restarts. event() keeps the last ones for the web UI and
 * hands each to its listeners: the console and syslog (rtl_guard.c), SNMP
 * traps, MQTT. Plain C, called from both halves of the firmware.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum event_type {
	EV_SYSTEM,		/* start, restart, rollback, updates */
	EV_LINK,		/* port, value: rtl_port.link */
	EV_LOGIN,		/* web, SSH */
	EV_LOGIN_FAILED,
	EV_DOT1X,
	EV_ERRDISABLE,		/* port, value: 1 disabled, 0 recovered */
	EV_CONFIG,
	EV_TYPES
};

struct event {
	uint32_t seq;
	uint32_t t_s;			/* uptime */
	uint8_t type;
	uint8_t port;			/* front panel, 0: none */
	int value;
	char text[96];
};

#define EVENTS_KEEP	48
#define EVENT_LISTENERS	4

void event(uint8_t type, uint8_t port, int value, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
bool event_listen(void (*fn)(const struct event *e));
const char *event_type_name(uint8_t type);
/* The kept ones after seq, oldest first, as a JSON array */
size_t events_json(char *buf, size_t max, uint32_t after);

/* Appends to buf as snprintf does, but returns the new length and never more than max - 1 */
size_t app(char *buf, size_t max, size_t n, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
/* A string as a JSON string, quotes included */
size_t app_json_str(char *buf, size_t max, size_t n, const char *s);

#endif
