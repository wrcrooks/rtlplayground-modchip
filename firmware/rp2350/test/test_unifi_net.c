/*
 * unifi.c in lwIP, against a UniFi Network controller (run_unifi_controller.py):
 *   test_unifi_net <controller's inform port on the host> <MAC> <store file>
 * Informs http://127.0.0.1:8080/inform, relayed to that port. Prints each
 * command the controller's configuration becomes ("RUN ..."), each event
 * ("EVENT ..."), "REBOOT" and "LED n". Lines on stdin are commands: "status",
 * "on", "off", "forget", "inform <url>", "link <port> <0|speed code>".
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lwip/init.h"
#include "mbedtls/platform_time.h"
#include "events.h"
#include "plat.h"
#include "store.h"
#include "unifi.h"
#include "relay.h"

char hostname[24] = "testswitch";
static uint8_t mac[6];
static const char *store_path;
static uint8_t links[6] = { 3, 0, 0, 6, 0, 0 };		/* port 1 at 1G, port 4 at 2.5G */

u32_t sys_now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
mbedtls_ms_time_t mbedtls_ms_time(void) { return sys_now(); }
static uint32_t t0;
uint32_t plat_uptime_s(void) { return (sys_now() - t0) / 1000; }	/* from 0, as after a restart */
uint32_t plat_heap_free(void) { return 150000; }
const struct ip6_addr *net_ip6_gw(struct netif *n, const struct ip6_addr *d) { return NULL; }
void net_dhcp_option(unsigned char option, unsigned char len, struct pbuf *p, unsigned short offset) { }
const uint8_t *rtl_mac(void) { return mac; }
void plat_reboot(void) { printf("REBOOT\n"); exit(3); }
void plat_sys_led(uint8_t state) { printf("LED %u\n", state); }
int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen)
{
	FILE *f = fopen("/dev/urandom", "rb");

	*olen = fread(out, 1, len, f);
	fclose(f);
	return 0;
}

static FILE *sector_file(int id, const char *mode)
{
	char path[300];

	snprintf(path, sizeof(path), "%s.%d", store_path, id);
	return fopen(path, mode);
}

void plat_sector_read(int id, uint8_t *dst, uint32_t len)
{
	FILE *f = sector_file(id, "rb");

	memset(dst, 0xff, len);
	if (f) {
		fread(dst, 1, len, f);
		fclose(f);
	}
}

bool plat_sector_write(int id, const uint8_t *src, uint32_t len)
{
	FILE *f = sector_file(id, "wb");

	fwrite(src, 1, len, f);
	fclose(f);
	return true;
}

/* A 4 + 2 switch like the MokerLink */
uint8_t rtl_ports_read(struct rtl_port *p, uint8_t max)
{
	for (uint8_t i = 0; i < 6 && i < max; i++) {
		memset(&p[i], 0, sizeof(p[i]));
		snprintf(p[i].name, sizeof(p[i].name), "Port %u", i + 1);
		p[i].sfp = i >= 4;
		p[i].enabled = true;
		p[i].link = links[i];
		p[i].in_octets = 1000000ULL * (i + 1);
		p[i].out_octets = 2000000ULL * (i + 1);
		p[i].in_ucast = 1000 * (i + 1);
	}
	return 6;
}

size_t rtl_run(char *line, char *out, size_t max, bool *ok)
{
	printf("RUN %s\n", line);
	*ok = true;
	return 0;
}

static void on_event(const struct event *e) { printf("EVENT %s\n", e->text); }

int main(int argc, char **argv)
{
	uint32_t last = 0;
	char line[200];
	size_t n = 0;
	static char st[600];

	if (argc != 4 || sscanf(argv[2], "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6)
		return 2;
	store_path = argv[3];
	t0 = sys_now();
	setvbuf(stdout, NULL, _IOLBF, 0);
	store_load();
	unifi_boot();
	lwip_init();
	netif_set_default(netif_list);
	relay_out(8080, atoi(argv[1]));
	event_listen(on_event);
	if (!store.unifi_url[0])
		unifi_inform("http://127.0.0.1:8080/inform");
	unifi_enable(true);	/* as the replayed configuration does */
	unifi_bridge_up();
	fcntl(0, F_SETFL, O_NONBLOCK);
	for (;;) {
		char c;
		unsigned a, b;

		while (read(0, &c, 1) == 1) {
			if (c != '\n') {
				if (n < sizeof(line) - 1)
					line[n++] = c;
				continue;
			}
			line[n] = 0;
			n = 0;
			if (!strcmp(line, "status")) {
				unifi_status(st, sizeof(st));
				printf("STATUS %s", st);
				printf("STATUS end\n");
			} else if (!strcmp(line, "on") || !strcmp(line, "off")) {
				unifi_enable(line[1] == 'n');
			} else if (!strcmp(line, "forget")) {
				unifi_forget();
			} else if (!strncmp(line, "inform ", 7)) {
				printf("INFORM %d\n", unifi_inform(line + 7));
			} else if (sscanf(line, "link %u %u", &a, &b) == 2 && a >= 1 && a <= 6) {
				links[a - 1] = b;
			}
		}
		relay_poll();
		if (plat_uptime_s() != last) {
			last = plat_uptime_s();
			unifi_second();
		}
		usleep(300);
	}
}
