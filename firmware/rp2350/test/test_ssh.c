/*
 * ssh.c and ssh_lwip.c in lwIP, for run_ssh.py and OpenSSH's client:
 *   test_ssh <port>
 * Commands here: echo <text>, big <bytes>, fail, login (the events so far).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lwip/init.h"
#include "events.h"
#include "plat.h"
#include "safety.h"
#include "ssh.h"
#include "store.h"
#include "net.h"
#include "relay.h"

struct altcp_pcb *altcp_tls_wrap(struct altcp_tls_config *config, struct altcp_pcb *inner) { return NULL; }

char hostname[24] = "testswitch";
static char last_restore[8192];
static uint8_t store_sector[4096];
static bool rebooted;

u32_t sys_now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
uint32_t plat_uptime_s(void) { return sys_now() / 1000; }
uint32_t plat_heap_free(void) { return getenv("HEAP_FREE") ? atoi(getenv("HEAP_FREE")) : 200000; }
uint32_t plat_random32(void) { return rand(); }
void plat_reboot(void) { rebooted = true; }
void plat_sector_read(int id, uint8_t *dst, uint32_t len) { memcpy(dst, store_sector, len); }
bool plat_sector_write(int id, const uint8_t *src, uint32_t len) { memcpy(store_sector, src, len); return true; }
const struct ip6_addr *net_ip6_gw(struct netif *n, const struct ip6_addr *d) { return NULL; }
void net_dhcp_option(unsigned char option, unsigned char len, struct pbuf *p, unsigned short offset) { }
int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen)
{
	FILE *f = fopen("/dev/urandom", "rb");

	*olen = fread(out, 1, len, f);
	fclose(f);
	return 0;
}
bool rtl_password_plain_ok(const char *p) { return !strcmp(p, "pass & word"); }
size_t config_backup(char *buf, size_t max) { return snprintf(buf, max, "# backup\nip 1.2.3.4\n"); }
bool config_restore(char *text, size_t len, unsigned minutes, char *msg, size_t max)
{
	snprintf(last_restore, sizeof(last_restore), "%u minutes, %zu bytes", minutes, len);
	snprintf(msg, max, strstr(text, "bad") ? "NO: line 1" : "OK: restored, restarting");
	return !strstr(text, "bad");
}

size_t rtl_run(char *line, char *out, size_t max, bool *ok)
{
	size_t n = 0;

	if (!strncmp(line, "echo ", 5))
		return snprintf(out, max, "%s\n", line + 5);
	if (!strncmp(line, "big ", 4)) {
		size_t want = atoi(line + 4);

		for (; n < want && n < max - 1; n++)
			out[n] = n % 64 == 63 ? '\n' : 'a' + n % 26;
		return n;
	}
	if (!strcmp(line, "events"))
		return events_json(out, max, 0);
	if (!strcmp(line, "restored"))
		return snprintf(out, max, "%s\n", last_restore);
	if (!strcmp(line, "ssh off")) {
		plat_ssh_enable(false);
		return 0;
	}
	if (!strcmp(line, "status"))
		return plat_ssh_status(out, max);
	*ok = false;
	return snprintf(out, max, "Error: unknown command\n");
}

int main(int argc, char **argv)
{
	uint32_t last = 0;

	(void)argc;
	setvbuf(stdout, NULL, _IOLBF, 0);
	lwip_init();
	store_load();
	if (!plat_ssh_enable(true))
		return 1;
	char st[120];
	plat_ssh_status(st, sizeof(st));
	fputs(st, stdout);
	relay_in(atoi(argv[1]), 22);
	for (;;) {
		relay_poll();
		ssh_conn_poll();
		if (plat_uptime_s() != last) {
			last = plat_uptime_s();
			ssh_conn_second();
			if (rebooted)
				puts("rebooted"), rebooted = false;
		}
		usleep(300);
	}
}
