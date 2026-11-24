/* MQTT, see telemetry.h. Commands "mqtt ..." and "prometheus ..." are in rtl_ext.c. */

#include <stdio.h>
#include <string.h>

#include "lwip/altcp_tls.h"
#include "lwip/apps/mqtt.h"
#include "lwip/netif.h"

#include "events.h"
#include "metrics.h"
#include "plat.h"
#include "telemetry.h"
#include "tls_client.h"

#define KEEP_ALIVE_S	30
#define RETRY_S		15
#define EVENTS_WAITING	8

static struct {
	bool on, server_set, tls;
	uint8_t ip[4];
	uint16_t port;			/* 0: 1883, or 8883 with TLS */
	uint16_t interval;
	char user[33], password[65], topic[33];
	uint8_t pin[TLS_PIN_LEN];
} cfg = { .interval = 30 };

static mqtt_client_t *client;
static struct altcp_tls_config *tls;
static bool want_restart;
static bool tls12, was_up;		/* TLS 1.2 is tried in turn, until one connects */
static bool new_tls;
static int last_error = -1;		/* mqtt_connection_status_t, -1: none yet */
static uint32_t retry_in, next_in;
static int due = -1;			/* what of a round goes out next, -1: nothing */
static bool hello_due;
static struct { uint8_t type, port; uint32_t seq; char text[96]; } ev[EVENTS_WAITING];
static unsigned ev_head, ev_n;

static const char *prefix(void)
{
	return cfg.topic[0] ? cfg.topic : hostname;
}


/* False: no room now, try later */
static bool publish(const char *topic, const char *text, bool retain)
{
	char t[64];

	snprintf(t, sizeof(t), "%s/%s", prefix(), topic);
	return mqtt_publish(client, t, text, strlen(text), 0, retain, NULL, NULL) == ERR_OK;
}


static void on_event(const struct event *e)
{
	unsigned i;

	if (!cfg.on)
		return;
	if (ev_n == EVENTS_WAITING) {	/* the oldest goes */
		ev_head = (ev_head + 1) % EVENTS_WAITING;
		ev_n--;
	}
	i = (ev_head + ev_n++) % EVENTS_WAITING;
	ev[i].type = e->type;
	ev[i].port = e->port;
	ev[i].seq = e->seq;
	strcpy(ev[i].text, e->text);
	if (e->type == EV_LINK)
		next_in = 0;		/* the port's state too */
}


static void connection(mqtt_client_t *c, void *arg, mqtt_connection_status_t status)
{
	(void)c; (void)arg;
	if (status == MQTT_CONNECT_ACCEPTED) {
		hello_due = true;
		next_in = 0;
		if (last_error)
			event(EV_SYSTEM, 0, 0, "MQTT connected");
	} else if (last_error != (int)status) {
		event(EV_SYSTEM, 0, 0, "MQTT connection %s (%d)", status < 256 ? "refused" : "lost", (int)status);
	}
	last_error = status;
	due = -1;
	retry_in = RETRY_S;
	if (status == MQTT_CONNECT_ACCEPTED) {
		was_up = true;
	} else if (status >= 256 && cfg.tls && !was_up) {
		tls12 = !tls12;
		new_tls = true;
		if (tls12)
			retry_in = 1;
	}
}


static void stop(void)
{
	if (client) {
		mqtt_disconnect(client);
		mqtt_client_free(client);
		client = NULL;
	}
	if (tls) {
		altcp_tls_free_config(tls);
		tls = NULL;
	}
	due = -1;
	hello_due = false;
}


static void start(void)
{
	struct mqtt_connect_client_info_t info = { 0 };
	char id[40], will[48];
	const uint8_t *m = rtl_mac();
	ip_addr_t ip;

	if (!client && !(client = mqtt_client_new()))
		return;
	if (new_tls && tls) {
		altcp_tls_free_config(tls);
		tls = NULL;
	}
	new_tls = false;
	if (cfg.tls && !tls && !(tls = tls_client_config(cfg.pin, false, tls12)))
		return;
	snprintf(id, sizeof(id), "modchip-%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
	snprintf(will, sizeof(will), "%s/status", prefix());
	info.client_id = id;
	info.client_user = cfg.user[0] ? cfg.user : NULL;
	info.client_pass = cfg.user[0] ? cfg.password : NULL;
	info.keep_alive = KEEP_ALIVE_S;
	info.will_topic = will;
	info.will_msg = "offline";
	info.will_retain = 1;
	info.tls_config = cfg.tls ? tls : NULL;
	IP_ADDR4(&ip, cfg.ip[0], cfg.ip[1], cfg.ip[2], cfg.ip[3]);
	mqtt_client_connect(client, &ip, cfg.port ? cfg.port : cfg.tls ? MQTT_TLS_PORT : MQTT_PORT, connection, NULL, &info);
}


/* The n-th message of a round; false when there is none */
static bool round_message(int n, char *topic, size_t tmax, char *b, size_t max)
{
	if (n == 0) {
		snprintf(topic, tmax, "switch");
		metrics_switch_json(b, max);
		return true;
	}
	snprintf(topic, tmax, "port/%d", n);
	return metrics_port_json(n, b, max);
}


static void pump(void)
{
	char b[500], topic[20];

	if (hello_due) {
		if (!publish("status", "online", true))
			return;
		hello_due = false;
	}
	while (ev_n) {
		size_t n = app(b, sizeof(b), 0, "{\"seq\":%lu,\"type\":\"%s\",\"port\":%u,\"text\":",
			       (unsigned long)ev[ev_head].seq, event_type_name(ev[ev_head].type), ev[ev_head].port);

		n = app_json_str(b, sizeof(b), n, ev[ev_head].text);
		app(b, sizeof(b), n, "}");
		if (!publish("event", b, false))
			return;
		ev_head = (ev_head + 1) % EVENTS_WAITING;
		ev_n--;
	}
	if (due < 0)
		return;
	while (round_message(due, topic, sizeof(topic), b, sizeof(b))) {
		if (!publish(topic, b, true))
			return;
		due++;
	}
	due = -1;
}


void telemetry_second(void)
{
	if (want_restart || (!cfg.on && client)) {
		want_restart = false;
		if (client && mqtt_client_is_connected(client))
			publish("status", "offline", true);	/* a clean end draws no last will */
		stop();
		tls12 = was_up = false;
		retry_in = 1;
		last_error = -1;
	}
	if (!cfg.on || !cfg.server_set)
		return;
	if (client && mqtt_client_is_connected(client)) {
		if (next_in)
			next_in--;
		if (!next_in && due < 0) {
			due = 0;
			next_in = cfg.interval;
		}
		pump();
		return;
	}
	if (!netif_default || !netif_is_up(netif_default) || ip4_addr_isany(netif_ip4_addr(netif_default)))
		return;
	if (retry_in) {
		retry_in--;
		return;
	}
	retry_in = RETRY_S;
	start();
}


/* ---- settings ---- */

void plat_mqtt_enable(bool on)
{
	if (on)
		event_listen(on_event);
	want_restart |= on != cfg.on;
	cfg.on = on;
}


void plat_mqtt_server(const uint8_t ip[4], uint16_t port)
{
	want_restart |= !cfg.server_set || memcmp(cfg.ip, ip, 4) || cfg.port != port;
	memcpy(cfg.ip, ip, 4);
	cfg.port = port;
	cfg.server_set = true;
}


bool plat_mqtt_login(const char *user, const char *password)
{
	if (strlen(user) >= sizeof(cfg.user) || strlen(password) >= sizeof(cfg.password))
		return false;
	want_restart |= strcmp(cfg.user, user) || strcmp(cfg.password, password);
	strcpy(cfg.user, user);
	strcpy(cfg.password, password);
	return true;
}


bool plat_mqtt_topic(const char *topic)
{
	if (strlen(topic) >= sizeof(cfg.topic) || strpbrk(topic, "+#"))
		return false;
	want_restart |= strcmp(cfg.topic, topic) != 0;
	strcpy(cfg.topic, topic);
	return true;
}


bool plat_mqtt_tls(const char *fingerprint)
{
	uint8_t pin[TLS_PIN_LEN] = { 0 };

	if (fingerprint && !tls_pin_parse(fingerprint, pin))
		return false;
	want_restart |= cfg.tls != (fingerprint != NULL) || memcmp(cfg.pin, pin, sizeof(pin));
	if (want_restart)
		stop();		/* the pin is in use */
	cfg.tls = fingerprint != NULL;
	memcpy(cfg.pin, pin, sizeof(pin));
	return true;
}


void plat_mqtt_interval(uint16_t s)
{
	cfg.interval = s;
}


size_t plat_mqtt_status(char *buf, size_t max)
{
	size_t n = app(buf, max, 0, "MQTT %s", !cfg.on ? "off" : client && mqtt_client_is_connected(client) ? "connected"
		       : "not connected");

	if (cfg.server_set)
		n = app(buf, max, n, ", server %u.%u.%u.%u:%u%s", cfg.ip[0], cfg.ip[1], cfg.ip[2], cfg.ip[3],
			cfg.port ? cfg.port : cfg.tls ? MQTT_TLS_PORT : MQTT_PORT, cfg.tls ? " over TLS" : "");
	return app(buf, max, n, ", topic %s, every %u s\n", prefix(), cfg.interval);
}
