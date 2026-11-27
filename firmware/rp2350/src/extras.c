/* /features.json, for web/extras.html */

#include <string.h>

#include "events.h"
#include "extras.h"
#include "mcast.h"
#include "plat.h"
#include "rtl_guard.h"
#include "safety.h"
#include "store.h"
#include "unifi.h"
#include "update.h"

/* A status text, and whether the service is on: the text starts "<name> off" if not */
static size_t service(char *buf, size_t max, size_t n, const char *name, const char *text, bool on)
{
	n = app(buf, max, n, "\"%s\":{\"on\":%d,\"text\":", name, on);
	n = app_json_str(buf, max, n, text);
	return app(buf, max, n, "}");
}


static bool says_on(const char *t)
{
	size_t k = strcspn(t, ",\n");

	return k < 4 || strncmp(t + k - 4, " off", 4);
}


static size_t services(char *buf, size_t max, size_t n)
{
	static const struct { const char *name; size_t (*status)(char *, size_t); } s[] = {
		{ "ip6", plat_ip6_status }, { "mqtt", plat_mqtt_status }, { "prom", plat_prometheus_status }, { "ssh", plat_ssh_status },
	};
	char t[800];		/* unifi_status() with every line: ~600 */

	plat_snmp_status(t, sizeof(t));
	n = service(buf, max, n, "snmp", t, plat_snmp_get(PLAT_SNMP_ENABLED) != NULL);
	for (unsigned i = 0; i < sizeof(s) / sizeof(s[0]); i++) {
		s[i].status(t, sizeof(t));
		n = app(buf, max, n, ",");
		n = service(buf, max, n, s[i].name, t, says_on(t));
	}
	/* UniFi: the inform URL as set (empty: auto), and whether a controller has it */
	unifi_status(t, sizeof(t));
	n = app(buf, max, n, ",\"unifi\":{\"on\":%d,\"adopted\":%d,\"url\":", unifi_enabled(), store.unifi_adopted ? 1 : 0);
	n = app_json_str(buf, max, n, store.unifi_url);
	n = app(buf, max, n, ",\"text\":");
	n = app_json_str(buf, max, n, t);
	return app(buf, max, n, "}");
}


size_t features_json(char *buf, size_t max)
{
	size_t n = app(buf, max, 0, "{\"reboot_left\":%lu,\"rollback_left\":%lu,\"guard\":",
		       (unsigned long)safety_reboot_left(), (unsigned long)safety_rollback_left());

	n += rtl_guard_json(buf + n, max - n);
	n = app(buf, max, n, ",\"mcast\":");
	n += mc_json(buf + n, max - n > 3000 ? 3000 : max - n);
	n = app(buf, max, n, ",\"update\":");
	n += update_json(buf + n, max - n);
	n = app(buf, max, n, ",\"uptime\":%lu,\"services\":{", (unsigned long)plat_uptime_s());
	n = services(buf, max, n);
	n = app(buf, max, n, "}");
	return app(buf, max, n, "}");
}
