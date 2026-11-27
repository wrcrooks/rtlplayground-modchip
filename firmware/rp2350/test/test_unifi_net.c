/*
 * unifi.c in lwIP, against a UniFi Network controller (run_unifi_controller.py):
 *   test_unifi_net <controller's inform port on the host> <MAC> <store file>
 * Informs http://127.0.0.1:8080/inform, relayed to that port. Prints each
 * command the controller's configuration becomes ("RUN ..."), each event
 * ("EVENT ..."), "REBOOT" and "LED n". Lines on stdin are commands: "status",
 * "on", "off", "forget", "inform <url>", "link <port> <0|speed code>", "ports <n>"
 * (0: none read, as while the bridge is down; UNIFI_PORTS sets it at the start),
 * "opt43 <hex>" (a DHCP option 43), "lldp <port> <hex>" (an LLDPDU's TLVs on that port).
 * UNIFI_NO_URL: no inform URL set at the start. "l2" answers as the switch's command, a
 * few addresses ("L2 READ" for each).
 * Names: "unifi" is 127.0.0.1 after two tries ("RESOLVE unifi" for each), the rest
 * aren't found.
 * "heap": "HEAP <live> <peak>", the C heap in use now and at most since the last
 * "heap" (malloc is wrapped: unifi.c's buffers and mbed TLS's; lwIP has its own).
 */
#include <fcntl.h>
#include <malloc.h>
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
#include "lldp_nbr.h"
#include "relay.h"

char hostname[24] = "testswitch";

/* -Wl,--wrap=malloc,... */
static size_t heap_live, heap_peak;
void *__real_malloc(size_t), *__real_calloc(size_t, size_t), *__real_realloc(void *, size_t);
void __real_free(void *);
static void *counted(void *p)
{
	if (p && (heap_live += malloc_usable_size(p)) > heap_peak)
		heap_peak = heap_live;
	return p;
}
void *__wrap_malloc(size_t n) { return counted(__real_malloc(n)); }
void *__wrap_calloc(size_t a, size_t b) { return counted(__real_calloc(a, b)); }
void __wrap_free(void *p) { if (p) heap_live -= malloc_usable_size(p); __real_free(p); }
void *__wrap_realloc(void *p, size_t n)
{
	if (p)
		heap_live -= malloc_usable_size(p);
	return counted(__real_realloc(p, n));
}
static uint8_t mac[6];
static const char *store_path;
static uint8_t links[6] = { 3, 0, 0, 6, 0, 0 };		/* port 1 at 1G, port 4 at 2.5G */
static uint8_t nports = 6;

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
void net_dhcp_append(struct dhcp_msg *msg, unsigned char type, unsigned short *len) { }
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
	for (uint8_t i = 0; i < nports && i < max; i++) {
		memset(&p[i], 0, sizeof(p[i]));
		snprintf(p[i].name, sizeof(p[i].name), "Port %u", i + 1);
		p[i].sfp = i >= 4;
		p[i].enabled = true;
		p[i].link = links[i];
		p[i].in_octets = 1000000ULL * (i + 1);
		p[i].out_octets = 2000000ULL * (i + 1);
		p[i].in_ucast = 1000 * (i + 1);
	}
	return nports < max ? nports : max;
}

size_t rtl_run(char *line, char *out, size_t max, bool *ok)
{
	if (!strcmp(line, "l2")) {
		/* as port_l2_learned() prints it: blank lines for entries not valid; the switch's own MAC, the CPU, a LAG */
		printf("L2 READ\n");
		*ok = true;
		/* the clients: aa:<the last three bytes of the switch's MAC>:00:0n, new for each switch */
		char c[12];
		snprintf(c, sizeof(c), "aa:%02x:%02x:%02x", mac[3], mac[4], mac[5]);
		return snprintf(out, max, "\n\tMAC\t\tVLAN\ttype\tport\n\n%s:00:01\t1\tlearned\t1\n"
				"%s:00:02\t10\tstatic\t1\n\n%s:00:04\t1\tlearned\t4\n"
				"%02x:%02x:%02x:%02x:%02x:%02x\t1\tlearned\tCPU\n%s:00:05\t1\tlearned\tLAG1\n"
				"%s:00:06\t20\tlearned\t6\n", c, c, c, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], c, c);
	}
	printf("RUN %s\n", line);
	*ok = true;
	return 0;
}

int rtl_resolve(const char *name, uint8_t ip[4])
{
	static int tries;

	printf("RESOLVE %s\n", name);
	if (strcmp(name, "unifi"))
		return -1;
	if (++tries % 3)
		return 0;
	memcpy(ip, (uint8_t[]){ 127, 0, 0, 1 }, 4);
	return 1;
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
	if (getenv("UNIFI_PORTS"))
		nports = atoi(getenv("UNIFI_PORTS"));
	event_listen(on_event);
	store_load();
	unifi_boot();
	lwip_init();
	netif_set_default(netif_list);
	relay_out(8080, atoi(argv[1]));
	if (!store.unifi_url[0] && !getenv("UNIFI_NO_URL"))
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
			} else if (!strcmp(line, "heap")) {
				printf("HEAP %zu %zu\n", heap_live, heap_peak);
				heap_peak = heap_live;
			} else if (!strcmp(line, "on") || !strcmp(line, "off")) {
				unifi_enable(line[1] == 'n');
			} else if (!strcmp(line, "forget")) {
				unifi_forget();
			} else if (!strncmp(line, "inform ", 7)) {
				printf("INFORM %d\n", unifi_inform(line + 7));
			} else if (sscanf(line, "lldp %u", &a) == 1) {
				uint8_t o[300];
				unsigned n = 0, v;
				const char *h = strchr(line + 5, ' ');

				for (h = h ? h + 1 : ""; n < sizeof(o) && sscanf(h, "%2x", &v) == 1; h += 2)
					o[n++] = v;
				lldp_nbr_rx(a, o, n, plat_uptime_s());
			} else if (!strncmp(line, "opt43 ", 6)) {
				uint8_t o[64];
				unsigned n = 0, v;

				for (const char *h = line + 6; n < sizeof(o) && sscanf(h, "%2x", &v) == 1; h += 2)
					o[n++] = v;
				unifi_dhcp_option43(o, n);
			} else if (sscanf(line, "ports %u", &a) == 1 && a <= 6) {
				nports = a;
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
