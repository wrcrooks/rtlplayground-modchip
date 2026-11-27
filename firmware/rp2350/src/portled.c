/* The port LEDs' colours, see portled.h */
#include <string.h>

#include "portled.h"

#define MA_FULL		20	/* a WS2812B colour at 255 */

void portled_defaults(struct portled_cfg *c)
{
	memset(c, 0, sizeof(*c));
	c->mode = PORTLED_VLAN;
	c->brightness = 30;
	c->pin = 0xff;
	c->budget_ma = 300;
}

/* Full saturation and value: hue in 0..1535 (six sectors of 256) */
static void hue_rgb(uint16_t h, uint8_t rgb[3])
{
	uint8_t x = h & 0xff, sector = (h >> 8) % 6;

	switch (sector) {
	case 0: rgb[0] = 255, rgb[1] = x, rgb[2] = 0; break;
	case 1: rgb[0] = 255 - x, rgb[1] = 255, rgb[2] = 0; break;
	case 2: rgb[0] = 0, rgb[1] = 255, rgb[2] = x; break;
	case 3: rgb[0] = 0, rgb[1] = 255 - x, rgb[2] = 255; break;
	case 4: rgb[0] = x, rgb[1] = 0, rgb[2] = 255; break;
	default: rgb[0] = 255, rgb[1] = 0, rgb[2] = 255 - x; break;
	}
}

void portled_vlan_rgb(const struct portled_cfg *c, uint16_t vid, uint8_t rgb[3])
{
	for (int i = 0; i < PORTLED_COLOURS; i++)
		if (c->colour[i].vid && c->colour[i].vid == vid) {
			memcpy(rgb, c->colour[i].rgb, 3);
			return;
		}
	/* the golden angle (137.5 of 360 degrees, 587 of 1536) apart: VLAN 1 blue, 2 orange, 3 green... */
	hue_rgb((uint16_t)((1024 + (uint32_t)(vid - 1) * 587) % 1536), rgb);
}

static const uint16_t speeds[PORTLED_SPEEDS] = { 10, 100, 1000, 2500, 5000, 10000 };
const char *const portled_speed_words[PORTLED_SPEEDS] = { "10m", "100m", "1g", "2g5", "5g", "10g" };

int portled_speed_index(uint16_t mbps)
{
	for (int i = 0; i < PORTLED_SPEEDS; i++)
		if (speeds[i] == mbps)
			return i;
	return -1;
}

int portled_speed_word(const char *w)
{
	for (int i = 0; i < PORTLED_SPEEDS; i++)
		if (!strcmp(portled_speed_words[i], w))
			return i;
	return -1;
}

void portled_speed_rgb(const struct portled_cfg *c, uint16_t mbps, uint8_t rgb[3])
{
	static const uint8_t def[PORTLED_SPEEDS][3] = {
		{ 255, 120, 0 }, { 255, 120, 0 }, { 0, 255, 0 }, { 0, 255, 120 }, { 0, 200, 255 }, { 0, 40, 255 },
	};
	int i = portled_speed_index(mbps);

	memset(rgb, mbps ? 255 : 0, 3);
	if (i >= 0)
		memcpy(rgb, c && (c->speed_set >> i & 1) ? c->speed[i] : def[i], 3);
}

bool portled_set_colour(struct portled_cfg *c, uint16_t vid, const uint8_t rgb[3])
{
	int fit = -1;

	for (int i = 0; i < PORTLED_COLOURS; i++) {
		if (c->colour[i].vid == vid) {
			fit = i;
			break;
		}
		if (!c->colour[i].vid && fit < 0)
			fit = i;
	}
	if (!rgb) {
		if (fit >= 0 && c->colour[fit].vid == vid)
			memset(&c->colour[fit], 0, sizeof(c->colour[fit]));
		return true;
	}
	if (fit < 0 || !vid)
		return false;
	c->colour[fit].vid = vid;
	memcpy(c->colour[fit].rgb, rgb, 3);
	return true;
}

void portled_locate(struct portled_locate *l, uint8_t port, uint32_t until_ms)
{
	if (port <= PORTLED_MAX)
		l->until[port] = until_ms;
}

static bool locating(const struct portled_locate *l, uint8_t port, uint32_t now)
{
	return l && l->until[port] && (int32_t)(l->until[port] - now) > 0;
}

uint32_t portled_ma(const uint8_t *grb, uint8_t n)
{
	uint32_t sum = 0;

	for (unsigned i = 0; i < 3u * n; i++)
		sum += grb[i];
	return (sum * MA_FULL + 254) / 255;
}

void portled_frame(const struct portled_cfg *c, const struct portled_locate *l, const struct portled_port *ports,
		   uint8_t n, uint32_t now, uint8_t *grb)
{
	bool flash_on = (now / PORTLED_LOCATE_MS) % 2 == 0;
	uint32_t sum = 0, level = c->brightness > 100 ? 100 : c->brightness;
	/* breathing: 20 % of the brightness up to all of it and back, by a triangle */
	uint32_t phase = now % PORTLED_BREATHE_MS, half = PORTLED_BREATHE_MS / 2;
	uint32_t breath = 20 + 80 * (phase < half ? phase : PORTLED_BREATHE_MS - phase) / half;

	if (n > PORTLED_MAX)
		n = PORTLED_MAX;
	memset(grb, 0, 3u * n);
	if (!c->on)
		return;
	for (uint8_t i = 0; i < n; i++) {
		uint8_t rgb[3] = { 0, 0, 0 };
		uint32_t pct = level;

		if (locating(l, 0, now) || locating(l, i + 1, now)) {
			if (flash_on)
				memset(rgb, 255, 3);
		} else if (ports[i].mbps) {
			if (c->mode == PORTLED_SPEED)
				portled_speed_rgb(c, ports[i].mbps, rgb);
			else
				portled_vlan_rgb(c, ports[i].pvid, rgb);
			if (c->breathe)
				pct = pct * breath / 100;
		}
		for (int k = 0; k < 3; k++) {
			uint8_t v = (uint16_t)rgb[k] * pct / 100;

			grb[3 * i + (k == 0 ? 1 : k == 1 ? 0 : 2)] = v;
			sum += v;
		}
	}
	/* down to the budget: what they draw scales with the sum */
	uint32_t ma = (sum * MA_FULL + 254) / 255;
	if (ma > c->budget_ma) {
		for (unsigned i = 0; i < 3u * n; i++)
			grb[i] = (uint32_t)grb[i] * c->budget_ma / ma;
	}
}
