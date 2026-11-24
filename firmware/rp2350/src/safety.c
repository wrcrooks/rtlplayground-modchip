/* Timed restart, configuration rollback and the restore check, see safety.h */

#include <stdio.h>
#include <string.h>

#include "events.h"
#include "plat.h"
#include "safety.h"
#include "store.h"

#define LINE_MAX_	126	/* CMD_BUF_SIZE - 2 */

static uint32_t reboot_left, rollback_left;
static uint8_t sector[PLAT_CONFIG_LEN];

static bool restore_snapshot(void)
{
	plat_sector_read(SECTOR_SNAPSHOT, sector, sizeof(sector));
	/* a torn snapshot must not become the configuration */
	if (!memchr(sector, 0, sizeof(sector)) || sector[0] == 0xff || !plat_config_write(sector, sizeof(sector)))
		return false;
	store.rollback = 0;
	store_save();
	return true;
}


void safety_boot(void)
{
	rollback_left = 0;
	if (!store.rollback)
		return;
	if (store.rollback == ROLLBACK_RESTORED && store.rollback_min && store.rollback_min <= SAFETY_MAX_MIN) {
		store.rollback = ROLLBACK_PENDING;	/* first: a start that crashes must not repeat this */
		if (store_save()) {
			rollback_left = (uint32_t)store.rollback_min * 60;
			event(EV_CONFIG, 0, store.rollback_min, "Rollback in %u min unless confirmed", store.rollback_min);
			return;
		}
	}
	if (restore_snapshot()) {
		event(EV_SYSTEM, 0, 0, "Configuration rolled back: the change was not confirmed");
	} else {
		event(EV_SYSTEM, 0, 0, "Configuration rollback failed: the kept copy is not usable");
		store.rollback = 0;
		store_save();
	}
}


void safety_reboot_in(uint16_t minutes)
{
	reboot_left = (uint32_t)minutes * 60;
	if (minutes)
		event(EV_SYSTEM, 0, minutes, "Restart in %u min", minutes);
	else
		event(EV_SYSTEM, 0, 0, "Timed restart cancelled");
}


uint32_t safety_reboot_left(void)
{
	return reboot_left;
}


bool safety_rollback_start(uint16_t minutes)
{
	if (!minutes || minutes > SAFETY_MAX_MIN)
		return false;
	if (!store.rollback) {
		plat_config_read(0, sector, sizeof(sector));
		if (!plat_sector_write(SECTOR_SNAPSHOT, sector, sizeof(sector)))
			return false;
		store.rollback = ROLLBACK_PENDING;
		if (!store_save()) {
			store.rollback = 0;
			return false;
		}
	}
	rollback_left = (uint32_t)minutes * 60;
	event(EV_CONFIG, 0, minutes, "Rollback in %u min unless confirmed", minutes);
	return true;
}


void safety_rollback_confirm(void)
{
	if (!store.rollback && !rollback_left)
		return;
	rollback_left = 0;
	store.rollback = 0;
	store_save();
	event(EV_CONFIG, 0, 0, "Configuration change confirmed");
}


uint32_t safety_rollback_left(void)
{
	return rollback_left;
}


void safety_second(void)
{
	if (rollback_left && !--rollback_left) {
		event(EV_CONFIG, 0, 0, "Change not confirmed: rolling the configuration back");
		restore_snapshot();	/* if it fails, safety_boot() tries again and says so */
		plat_reboot();
	}
	if (reboot_left && !--reboot_left) {
		event(EV_SYSTEM, 0, 0, "Timed restart");
		plat_reboot();
	}
}


/* The commands a configuration may hold; run_config_words.py checks them
 * against the parser's */
static const char *const words[] = {
	"acl", "backup", "bw", "dhcp-snoop", "dns", "dot1x", "eee", "errdisable", "gpio", "gw", "hostname", "https",
	"igmp", "ingress", "ip", "ip6", "isolate", "l2", "lacp", "lag", "laghash", "leds", "lldp", "mac", "mcsnoop",
	"mirror", "mqtt", "mtu", "netmask", "ntp", "passwd", "poe", "port", "prometheus", "pvid", "qos", "session",
	"sfp", "snmp", "ssh", "storm", "stp", "syslog", "t3", "time", "vlan", NULL
};

int config_check(char *text, size_t len, char *err, size_t err_len)
{
	size_t in = 0, out = 0;
	unsigned line = 0;
	bool any = false;

	while (in < len && text[in]) {
		size_t end = in, n, w;

		line++;
		while (end < len && text[end] && text[end] != '\n')
			end++;
		n = end - in;
		while (n && (text[in + n - 1] == '\r' || text[in + n - 1] == ' ' || text[in + n - 1] == '\t'))
			n--;
		const char *p = text + in;
		in = end < len && text[end] == '\n' ? end + 1 : end;
		while (n && (*p == ' ' || *p == '\t')) {
			p++;
			n--;
		}
		if (!n || *p == '#')
			continue;
		if (n > LINE_MAX_) {
			snprintf(err, err_len, "line %u is too long", line);
			return -1;
		}
		for (size_t i = 0; i < n; i++)
			if ((unsigned char)p[i] < 0x20 || p[i] == 0x7f) {
				snprintf(err, err_len, "line %u holds a control character", line);
				return -1;
			}
		for (w = 0; w < n && p[w] != ' '; w++)
			;
		int k = 0;
		while (words[k] && (strlen(words[k]) != w || memcmp(words[k], p, w)))
			k++;
		if (!words[k]) {
			snprintf(err, err_len, "line %u: \"%.*s\" is not a configuration command", line, (int)(w > 20 ? 20 : w), p);
			return -1;
		}
		memmove(text + out, p, n);	/* never ahead of what is still to read */
		out += n;
		text[out++] = '\n';
		any = true;
	}
	if (!any) {
		snprintf(err, err_len, "the file holds no commands");
		return -1;
	}
	return out;
}


size_t config_backup(char *buf, size_t max)
{
	size_t n = app(buf, max, 0, "# %s: configuration backup. Holds passwords and secrets.\n", hostname);

	plat_config_read(0, sector, sizeof(sector));
	for (size_t i = 0; i < sizeof(sector) && sector[i] && sector[i] != 0xff && n < max - 2; i++)
		buf[n++] = sector[i];
	if (buf[n - 1] != '\n')
		buf[n++] = '\n';
	buf[n] = 0;
	return n;
}


bool config_restore(char *text, size_t len, unsigned minutes, char *msg, size_t max)
{
	char err[80];
	int k = config_check(text, len, err, sizeof(err));

	if (k < 0 || k + 1 > (int)sizeof(sector)) {
		app(msg, max, 0, "NO: %s", k < 0 ? err : "the file is too long");
		return false;
	}
	if (minutes > SAFETY_MAX_MIN || (minutes && !safety_rollback_start(minutes))) {
		app(msg, max, 0, "NO: the rollback could not be prepared");
		return false;
	}
	memset(sector, 0xff, sizeof(sector));
	memcpy(sector, text, k);
	sector[k] = 0;
	if (!plat_config_write(sector, sizeof(sector))) {
		app(msg, max, 0, "NO: writing to the flash failed");
		return false;
	}
	if (minutes) {
		store.rollback = ROLLBACK_RESTORED;
		store.rollback_min = minutes;
		if (!store_save()) {
			store.rollback = ROLLBACK_PENDING;
			app(msg, max, 0, "NO: the rollback could not be prepared, the restart will undo this");
			return false;
		}
	}
	event(EV_CONFIG, 0, 0, "Configuration restored from a file");
	app(msg, max, 0, "OK: restored, restarting");
	return true;
}
