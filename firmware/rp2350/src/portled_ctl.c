/*
 * The port LED board's settings and state: what portled.c draws, from the
 * ports once a second, sent by portled_pio.c (the sim stubs that). The settings
 * are kept in the store.
 */
#include <stdio.h>
#include <string.h>

#include "events.h"
#include "plat.h"
#include "portled.h"
#include "portled_ctl.h"
#include "store.h"

#define FRAME_MS	20		/* the most often a frame goes out */
#define REFRESH_MS	1000		/* and the least: an LED that missed one catches up */
#define UNIFI_LOCATE_MS	(24u * 3600 * 1000)	/* until the controller says stop */

static struct portled_locate loc;
static struct portled_port ports[PORTLED_MAX];
static uint8_t nports, last[3 * PORTLED_MAX], pin_now = 0xff;
static uint32_t sent_at, frames;
static bool sent_dark;

static const uint16_t speed_mbps[] = { 0, 10, 100, 1000, 500, 10000, 2500, 5000 };

/* The data line as the settings want it */
static void pin_up(void)
{
	uint8_t want = store.portled.on ? store.portled.pin : 0xff;

	if (want == pin_now)
		return;
	if (pin_now != 0xff) {
		if (!sent_dark) {	/* dark before letting go of the line */
			memset(last, 0, sizeof(last));
			portled_hw_send(last, PORTLED_MAX);
		}
		portled_hw_pin(0xff);
	}
	pin_now = 0xff;
	if (want != 0xff && portled_hw_pin(want))
		pin_now = want;
	sent_at = plat_ms() - REFRESH_MS;
}

void portled_boot(void)
{
	pin_up();
}

void portled_second(void)
{
	struct rtl_port all[RTL_MAX_PORTS];
	uint8_t n;

	if (pin_now == 0xff)
		return;
	n = rtl_ports_read(all, RTL_MAX_PORTS);
	nports = n > PORTLED_MAX ? PORTLED_MAX : n;
	for (uint8_t i = 0; i < nports; i++) {
		ports[i].mbps = all[i].link < sizeof(speed_mbps) / sizeof(speed_mbps[0]) ? speed_mbps[all[i].link] : 0;
		ports[i].pvid = rtl_port_pvid(i + 1);
	}
}

void portled_poll(void)
{
	uint8_t f[3 * PORTLED_MAX];
	uint32_t now = plat_ms();

	if (pin_now == 0xff || now - sent_at < FRAME_MS || !nports)
		return;
	portled_frame(&store.portled, &loc, ports, nports, now, f);
	if (!memcmp(f, last, 3u * nports) && now - sent_at < REFRESH_MS)
		return;
	memcpy(last, f, 3u * nports);
	portled_hw_send(f, nports);
	sent_dark = portled_ma(f, nports) == 0;
	sent_at = now;
	frames++;
}

void portled_unifi_locate(bool on)
{
	portled_locate(&loc, 0, on ? plat_ms() + UNIFI_LOCATE_MS : 0);
}

/* Kept only when changed: UniFi's configuration sends them all again at each start */
static bool save(const struct portled_cfg *was)
{
	pin_up();
	return !memcmp(was, &store.portled, sizeof(*was)) || store_save();
}

#define SET(field, v) do { \
		struct portled_cfg was_ = store.portled; \
		store.portled.field = (v); \
		return save(&was_); \
	} while (0)

bool portled_enable(bool on)
{
	SET(on, on);
}

bool portled_set_pin(uint8_t pin)
{
	if (pin != 0xff && !portled_hw_pin_ok(pin))
		return false;
	if (pin_now != 0xff && pin != store.portled.pin) {	/* let go of the old one first */
		uint8_t on = store.portled.on;

		store.portled.on = 0;
		pin_up();
		store.portled.on = on;
	}
	SET(pin, pin);
}

bool portled_set_mode(uint8_t mode)
{
	SET(mode, mode);
}

bool portled_set_brightness(uint8_t pct)
{
	if (pct < 1 || pct > 100)
		return false;
	SET(brightness, pct);
}

bool portled_set_budget(uint16_t ma)
{
	if (ma < 20 || ma > PORTLED_BUDGET_MAX)
		return false;
	SET(budget_ma, ma);
}

bool portled_set_breathe(bool on)
{
	SET(breathe, on);
}

bool portled_set_speed_colour(int speed, const uint8_t rgb[3])
{
	struct portled_cfg was = store.portled;

	if (speed < 0 || speed >= PORTLED_SPEEDS)
		return false;
	if (rgb) {
		memcpy(store.portled.speed[speed], rgb, 3);
		store.portled.speed_set |= 1 << speed;
	} else {
		memset(store.portled.speed[speed], 0, 3);
		store.portled.speed_set &= ~(1 << speed);
	}
	return save(&was);
}

bool portled_set_vlan_colour(uint16_t vid, const uint8_t rgb[3])
{
	struct portled_cfg was = store.portled;

	if (vid < 1 || vid > 4094 || !portled_set_colour(&store.portled, vid, rgb))
		return false;
	return save(&was);
}

bool portled_locate_port(uint8_t port, uint16_t seconds)
{
	if (port > PORTLED_MAX)
		return false;
	portled_locate(&loc, port, seconds ? plat_ms() + seconds * 1000u : 0);
	return true;
}

size_t portled_status(char *buf, size_t max)
{
	const struct portled_cfg *c = &store.portled;
	uint32_t now = plat_ms();
	size_t n;

	n = app(buf, max, 0, "Port LEDs %s, ", c->on ? "on" : "off");
	if (c->pin == 0xff)
		n = app(buf, max, n, "no data pin set");
	else
		n = app(buf, max, n, "data on GPIO %u%s", c->pin, c->on && pin_now == 0xff ? " (could not be had)" : "");
	n = app(buf, max, n, "\n  by %s, brightness %u %%%s, at most %u mA\n", c->mode == PORTLED_SPEED ? "speed" : "VLAN",
		c->brightness, c->breathe ? ", breathing" : "", c->budget_ma);
	for (int i = 0; i < PORTLED_SPEEDS; i++)
		if (c->speed_set >> i & 1)
			n = app(buf, max, n, "  %s: %02x%02x%02x\n", portled_speed_words[i], c->speed[i][0], c->speed[i][1], c->speed[i][2]);
	for (int i = 0; i < PORTLED_COLOURS; i++)
		if (c->colour[i].vid)
			n = app(buf, max, n, "  VLAN %u: %02x%02x%02x\n", c->colour[i].vid, c->colour[i].rgb[0],
				c->colour[i].rgb[1], c->colour[i].rgb[2]);
	for (uint8_t p = 0; p <= PORTLED_MAX; p++)
		if (loc.until[p] && (int32_t)(loc.until[p] - now) > 0)
			n = p ? app(buf, max, n, "  locating port %u, %lu s left\n", p, (unsigned long)(loc.until[p] - now + 999) / 1000)
			      : app(buf, max, n, "  locating the switch (UniFi)\n");
	if (pin_now != 0xff)
		n = app(buf, max, n, "  %u LEDs drawing about %lu mA, %lu frames sent\n", nports,
			(unsigned long)portled_ma(last, nports), (unsigned long)frames);
	return n;
}
