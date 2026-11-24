/*
 * telemetry.c and tls_client.c in lwIP, for run_mqtt.py's broker or a real one:
 *   test_mqtt <broker's port> <setting>...
 * Settings as "tls=<fingerprint>", "login=<user>,<password>". Lines on stdin
 * become events; "!topic x" changes the topic, "!off" switches off.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lwip/init.h"
#include "mbedtls/platform_time.h"
#include "events.h"
#include "metrics.h"
#include "plat.h"
#include "telemetry.h"
#include "tls_client.h"
#include "relay.h"

char hostname[24] = "testswitch";

u32_t sys_now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
mbedtls_ms_time_t mbedtls_ms_time(void) { return sys_now(); }
uint32_t plat_uptime_s(void) { return sys_now() / 1000; }
const struct ip6_addr *net_ip6_gw(struct netif *n, const struct ip6_addr *d) { return NULL; }
void net_dhcp_option(unsigned char option, unsigned char len, struct pbuf *p, unsigned short offset) { }
bool tls_identity(struct tls_identity *id) { return false; }
const uint8_t *rtl_mac(void) { return (const uint8_t *)"\x06\x0f\x00\xdd\x3b\x3e"; }
size_t metrics_switch_json(char *buf, size_t max) { return snprintf(buf, max, "{\"temp\":45.5}"); }
bool metrics_port_json(int n, char *buf, size_t max)
{
	/* as long as they get */
	return n <= 10 && snprintf(buf, max, "{\"name\":\"%0*d\",\"rx_bytes\":%d}", 380, n, n) > 0;
}
int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen)
{
	FILE *f = fopen("/dev/urandom", "rb");

	*olen = fread(out, 1, len, f);
	fclose(f);
	return 0;
}

int main(int argc, char **argv)
{
	uint32_t last = 0;
	char line[200];
	size_t n = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	lwip_init();
	netif_set_default(netif_list);
	relay_out(1883, atoi(argv[1]));
	plat_mqtt_server((const uint8_t *)"\x7f\0\0\x01", 1883);
	plat_mqtt_interval(5);
	for (int i = 2; i < argc; i++) {
		char *v = strchr(argv[i], '=') + 1, *c;

		if (!strncmp(argv[i], "tls=", 4) && !plat_mqtt_tls(v))
			return 2;
		if (!strncmp(argv[i], "login=", 6)) {
			c = strchr(v, ',');
			*c = 0;
			plat_mqtt_login(v, c + 1);
		}
	}
	plat_mqtt_enable(true);
	fcntl(0, F_SETFL, O_NONBLOCK);
	for (;;) {
		char c;

		while (read(0, &c, 1) == 1) {
			if (c != '\n') {
				line[n++] = c;
				continue;
			}
			line[n] = 0;
			n = 0;
			if (!strncmp(line, "!topic ", 7))
				plat_mqtt_topic(line + 7);
			else if (!strcmp(line, "!off"))
				plat_mqtt_enable(false);
			else
				event(EV_SYSTEM, 2, 0, "%s", line);
		}
		relay_poll();
		if (plat_uptime_s() != last) {
			last = plat_uptime_s();
			telemetry_second();
		}
		usleep(300);
	}
}
