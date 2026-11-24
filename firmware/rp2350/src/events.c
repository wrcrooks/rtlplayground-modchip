/* The event log, see events.h */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "events.h"
#include "plat.h"

static struct event kept[EVENTS_KEEP];
static uint32_t seq;
static void (*listeners[EVENT_LISTENERS])(const struct event *e);

const char *event_type_name(uint8_t type)
{
	static const char *const names[EV_TYPES] = {
		"system", "link", "login", "login-failed", "dot1x", "errdisable", "config"
	};

	return type < EV_TYPES ? names[type] : "?";
}


bool event_listen(void (*fn)(const struct event *e))
{
	for (int i = 0; i < EVENT_LISTENERS; i++) {
		if (listeners[i] == fn)
			return true;
		if (!listeners[i]) {
			listeners[i] = fn;
			return true;
		}
	}
	return false;
}


void event(uint8_t type, uint8_t port, int value, const char *fmt, ...)
{
	struct event *e = &kept[seq % EVENTS_KEEP];
	struct event copy;
	va_list ap;

	memset(e, 0, sizeof(*e));
	e->seq = ++seq;
	e->t_s = plat_uptime_s();
	e->type = type;
	e->port = port;
	e->value = value;
	va_start(ap, fmt);
	vsnprintf(e->text, sizeof(e->text), fmt, ap);
	va_end(ap);
	/* what came from a frame or a login may hold anything */
	for (char *p = e->text; *p; p++)
		if (*p < 0x20 || *p > 0x7e)
			*p = '.';
	copy = *e;	/* a listener may log an event of its own */
	for (int i = 0; i < EVENT_LISTENERS && listeners[i]; i++)
		listeners[i](&copy);
}


size_t app(char *buf, size_t max, size_t n, const char *fmt, ...)
{
	va_list ap;
	int k;

	if (n + 1 >= max) {
		if (max)
			buf[max - 1] = 0;
		return n;
	}
	va_start(ap, fmt);
	k = vsnprintf(buf + n, max - n, fmt, ap);
	va_end(ap);
	if (k < 0)
		return n;
	return n + k < max ? n + k : max - 1;
}


size_t app_json_str(char *buf, size_t max, size_t n, const char *s)
{
	n = app(buf, max, n, "\"");
	for (; *s; s++) {
		unsigned char c = *s;
		if (c == '"' || c == '\\')
			n = app(buf, max, n, "\\%c", c);
		else if (c < 0x20 || c > 0x7e)
			n = app(buf, max, n, "\\u%04x", c);
		else
			n = app(buf, max, n, "%c", c);
	}
	return app(buf, max, n, "\"");
}


size_t events_json(char *buf, size_t max, uint32_t after)
{
	uint32_t first = seq > EVENTS_KEEP ? seq - EVENTS_KEEP : 0;
	size_t n = app(buf, max, 0, "[");
	bool comma = false;

	if (after > first)
		first = after < seq ? after : seq;
	for (uint32_t s = first; s < seq; s++) {
		const struct event *e = &kept[s % EVENTS_KEEP];

		if (n + 200 > max)
			break;
		n = app(buf, max, n, "%s{\"seq\":%lu,\"t\":%lu,\"type\":\"%s\",\"port\":%u,\"text\":", comma ? "," : "",
			(unsigned long)e->seq, (unsigned long)e->t_s, event_type_name(e->type), e->port);
		n = app_json_str(buf, max, n, e->text);
		n = app(buf, max, n, "}");
		comma = true;
	}
	return app(buf, max, n, "]");
}
