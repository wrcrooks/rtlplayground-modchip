/* Events, stored values, timed restart, rollback and the restore check */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "events.h"
#include "plat.h"
#include "safety.h"
#include "store.h"

static int failures;
static uint32_t now;
static uint8_t config[PLAT_CONFIG_LEN], sectors[2][4096];
static int reboots, sector_fail;

uint32_t plat_uptime_s(void) { return now; }
void plat_reboot(void) { reboots++; }
char hostname[24] = "sw";
void plat_config_read(uint32_t off, uint8_t *dst, uint32_t len) { memcpy(dst, config + off, len); }
bool plat_config_write(const uint8_t *src, uint32_t len) { memcpy(config, src, len); return true; }
void plat_sector_read(int id, uint8_t *dst, uint32_t len) { memcpy(dst, sectors[id], len); }
bool plat_sector_write(int id, const uint8_t *src, uint32_t len)
{
	if (sector_fail)
		return false;
	memset(sectors[id], 0xff, 4096);
	memcpy(sectors[id], src, len);
	return true;
}

static void check(bool ok, const char *fmt, ...)
{
	va_list ap;

	printf("%s  ", ok ? "PASS" : "FAIL");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	failures += !ok;
}

static void set_config(const char *t)
{
	memset(config, 0xff, sizeof(config));
	strcpy((char *)config, t);
}

static void seconds(int n)
{
	while (n--) {
		now++;
		safety_second();
	}
}

static void restart(void)
{
	memset(&store, 0x55, sizeof(store));
	store_load();
	safety_boot();
}

static int heard;
static char last[96];
static void listener(const struct event *e) { heard++; strcpy(last, e->text); }

static int cc(const char *in, char *out, char *err)
{
	char b[4096];
	int n;

	strcpy(b, in);
	err[0] = 0;
	n = config_check(b, strlen(in), err, 80);
	if (n >= 0) {
		memcpy(out, b, n);
		out[n] = 0;
	}
	return n;
}

int main(int argc, char **argv)
{
	static char j[8192], out[4096], err[80];

	if (argc > 1) {	/* a word per argument: is it accepted? */
		for (int i = 1; i < argc; i++)
			printf("%s %s\n", cc(argv[i], out, err) > 0 ? "yes" : "no", argv[i]);
		return 0;
	}

	/* events */
	check(event_listen(listener) && event_listen(listener), "a listener registers once");
	event(EV_LINK, 3, 1, "Port %u link %s", 3, "up");
	check(heard == 1 && !strcmp(last, "Port 3 link up"), "an event reaches its listener once");
	event(EV_LOGIN, 0, 0, "user \"a\\b\"\n\x80<");
	events_json(j, sizeof(j), 0);
	check(strstr(j, "\"text\":\"user \\\"a\\\\b\\\"..<\"") != NULL, "quotes, backslashes and control characters are made safe");
	for (int i = 0; i < 200; i++)
		event(EV_SYSTEM, 0, 0, "n%d", i);
	events_json(j, sizeof(j), 0);
	check(strstr(j, "\"n199\"") && strstr(j, "\"n152\"") && !strstr(j, "\"n151\"") && j[strlen(j) - 1] == ']', "the last 48 are kept");
	events_json(j, sizeof(j), 201);
	check(strstr(j, "\"n199\"") && !strstr(j, "\"n198\""), "only those after a sequence number");
	events_json(j, sizeof(j), 999);
	check(!strcmp(j, "[]"), "none after a later one");
	for (size_t m = 1; m < 400; m++) {
		char *t = malloc(m);
		size_t n = events_json(t, m, 0);
		if (n >= m || t[n]) {
			check(false, "a %zu byte buffer holds", m);
			break;
		}
		free(t);
	}

	/* store */
	memset(sectors, 0xff, sizeof(sectors));
	store_load();
	check(!store.rollback && !store.snmp_boots && !store.ssh_key_len, "a blank sector gives empty values");
	store.snmp_boots = 7;
	store_save();
	restart();
	check(store.snmp_boots == 7, "values come back");

	/* timed restart */
	safety_reboot_in(2);
	seconds(119);
	check(!reboots && safety_reboot_left() == 1, "restart not yet");
	seconds(1);
	check(reboots == 1 && !safety_reboot_left(), "restart in 2 min");
	safety_reboot_in(1);
	safety_reboot_in(0);
	seconds(100);
	check(reboots == 1, "cancelled");

	/* rollback */
	set_config("ip 10.0.0.1\n");
	check(!safety_rollback_start(0) && !safety_rollback_start(1441), "minutes out of range");
	check(safety_rollback_start(1), "rollback armed");
	set_config("ip 10.9.9.9\n");
	seconds(59);
	check(reboots == 1, "not yet");
	seconds(1);
	check(reboots == 2 && !strcmp((char *)config, "ip 10.0.0.1\n") && !store.rollback, "not confirmed: the old configuration and a restart");
	restart();
	check(!strcmp((char *)config, "ip 10.0.0.1\n"), "and it stays");

	safety_rollback_start(5);
	set_config("ip 10.9.9.9\n");
	seconds(30);
	safety_rollback_confirm();
	seconds(1000);
	restart();
	check(reboots == 2 && !strcmp((char *)config, "ip 10.9.9.9\n"), "confirmed: the new one stays");

	set_config("ip 10.0.0.1\n");
	safety_rollback_start(5);
	set_config("ip 10.9.9.9\n");
	restart();
	check(!strcmp((char *)config, "ip 10.0.0.1\n") && !store.rollback, "a restart before confirming rolls back");
	seconds(1000);

	set_config("ip 10.0.0.1\n");
	safety_rollback_start(5);
	set_config("ip 10.9.9.9\n");
	safety_rollback_start(9);	/* extended: still back to the first */
	restart();
	check(!strcmp((char *)config, "ip 10.0.0.1\n"), "extending keeps the first copy");
	seconds(1000);

	set_config("ip 10.0.0.1\n");
	safety_rollback_start(5);
	memset(sectors[SECTOR_SNAPSHOT], 0xff, 4096);
	set_config("ip 10.9.9.9\n");
	restart();
	check(!strcmp((char *)config, "ip 10.9.9.9\n") && !store.rollback, "a torn copy is not restored");
	sector_fail = 1;
	check(!safety_rollback_start(5) && !store.rollback, "no rollback when the copy cannot be kept");
	sector_fail = 0;

	/* a restore with time to confirm: its own restart isn't the one that rolls back */
	{
		char t[200], m[110];

		#define RESTORE(min) (strcpy(t, "ip 10.7.7.7\n"), config_restore(t, strlen(t), min, m, sizeof(m)))
		set_config("ip 10.0.0.1\n");
		check(RESTORE(2), "restored with 2 minutes to confirm: %s", m);
		restart();
		check(!strcmp((char *)config, "ip 10.7.7.7\n") && safety_rollback_left() == 120, "in effect after the restart, the time starts there");
		seconds(60);
		safety_rollback_confirm();
		seconds(1000);
		restart();
		check(!strcmp((char *)config, "ip 10.7.7.7\n"), "confirmed: it stays");
		set_config("ip 10.0.0.1\n");
		RESTORE(2);
		restart();
		seconds(121);
		check(!strcmp((char *)config, "ip 10.0.0.1\n") && !store.rollback, "not confirmed: the old one is back");
		RESTORE(2);
		restart();
		restart();
		check(!strcmp((char *)config, "ip 10.0.0.1\n") && !store.rollback, "a second restart rolls back");
		RESTORE(0);
		restart();
		check(!strcmp((char *)config, "ip 10.7.7.7\n") && !store.rollback && !safety_rollback_left(), "without minutes it just stays");
		set_config("ip 10.0.0.1\n");
	}

	/* the restore check */
	check(cc("# c\r\n\r\n  ip 10.0.0.1  \r\nvlan 5 t 1\n#x\nport 1 off", out, err) > 0
	      && !strcmp(out, "ip 10.0.0.1\nvlan 5 t 1\nport 1 off\n"), "comments, blanks and CRLF go: [%s]", out);
	check(cc("ip 1.2.3.4\nrm -rf\n", out, err) < 0 && strstr(err, "line 2"), "an unknown command is refused: %s", err);
	check(cc("reset\n", out, err) < 0 && cc("reboot now\n", out, err) < 0 && cc("flash e\n", out, err) < 0,
	      "commands that act rather than configure are refused");
	check(cc("ipx 1\n", out, err) < 0 && cc("i 1\n", out, err) < 0, "a prefix is not a match");
	check(cc("", out, err) < 0 && cc("# only\n", out, err) < 0, "an empty file is refused");
	memset(j, 'a', 127);
	memcpy(j, "hostname ", 9);
	j[127] = 0;
	check(cc(j, out, err) < 0, "a line over 126 is refused");
	j[126] = 0;
	check(cc(j, out, err) == 127, "126 is taken");
	check(cc("hostname a\x01z\n", out, err) < 0, "a control character is refused");

	printf("\n%s\n", failures ? "FAILED" : "all passed");
	return failures != 0;
}
