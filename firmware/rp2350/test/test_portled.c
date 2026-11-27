/* portled.c: colours, locating, brightness and the current budget */
#include <stdio.h>
#include <string.h>

#include "portled.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static bool same(const uint8_t *a, uint8_t r, uint8_t g, uint8_t b)
{
	return a[0] == r && a[1] == g && a[2] == b;
}

int main(void)
{
	struct portled_cfg c;
	struct portled_locate l;
	struct portled_port p[PORTLED_MAX];
	uint8_t f[3 * PORTLED_MAX], rgb[3], other[3];

	portled_defaults(&c);
	memset(&l, 0, sizeof(l));
	memset(p, 0, sizeof(p));
	CHECK(!c.on && c.mode == PORTLED_VLAN && c.brightness == 30 && c.pin == 0xff && c.budget_ma == 300, "defaults");

	/* VLAN colours: VLAN 1 blue; neighbours far apart; all bright */
	portled_vlan_rgb(&c, 1, rgb);
	CHECK(same(rgb, 0, 0, 255), "VLAN 1 blue: %u %u %u", rgb[0], rgb[1], rgb[2]);
	for (uint16_t v = 1; v < 4094; v++) {
		portled_vlan_rgb(&c, v, rgb);
		portled_vlan_rgb(&c, v + 1, other);
		int d = 0;
		for (int k = 0; k < 3; k++)
			d += rgb[k] > other[k] ? rgb[k] - other[k] : other[k] - rgb[k];
		if (d < 150 || (rgb[0] | rgb[1] | rgb[2]) != 255) {
			CHECK(false, "VLAN %u and %u: %u %u %u, %u %u %u", v, v + 1, rgb[0], rgb[1], rgb[2], other[0], other[1], other[2]);
			break;
		}
	}
	/* a VLAN's own colour, dropped, no room */
	CHECK(portled_set_colour(&c, 10, (const uint8_t[]){ 1, 2, 3 }), "set a colour");
	portled_vlan_rgb(&c, 10, rgb);
	CHECK(same(rgb, 1, 2, 3), "VLAN 10's own colour");
	CHECK(portled_set_colour(&c, 10, (const uint8_t[]){ 4, 5, 6 }), "changed");
	portled_vlan_rgb(&c, 10, rgb);
	CHECK(same(rgb, 4, 5, 6) && c.colour[1].vid == 0, "changed in place");
	for (uint16_t v = 100; v < 100 + PORTLED_COLOURS - 1; v++)
		CHECK(portled_set_colour(&c, v, (const uint8_t[]){ 9, 9, 9 }), "room for %u", v);
	CHECK(!portled_set_colour(&c, 200, (const uint8_t[]){ 9, 9, 9 }), "no room for one more");
	CHECK(!portled_set_colour(&c, 0, (const uint8_t[]){ 9, 9, 9 }) || 1, "VLAN 0");
	CHECK(portled_set_colour(&c, 10, NULL), "dropped");
	portled_vlan_rgb(&c, 10, rgb);
	CHECK(!same(rgb, 4, 5, 6), "its own colour gone");
	CHECK(portled_set_colour(&c, 200, (const uint8_t[]){ 7, 7, 7 }), "room again");
	portled_defaults(&c);
	CHECK(!portled_set_colour(&c, 0, (const uint8_t[]){ 9, 9, 9 }), "VLAN 0 has none");

	/* speeds */
	portled_speed_rgb(NULL, 1000, rgb);
	CHECK(same(rgb, 0, 255, 0), "1G green");
	portled_speed_rgb(NULL, 100, rgb);
	CHECK(same(rgb, 255, 120, 0), "100M amber");
	portled_speed_rgb(NULL, 10000, rgb);
	CHECK(same(rgb, 0, 40, 255), "10G blue");
	portled_speed_rgb(NULL, 500, rgb);
	CHECK(same(rgb, 255, 255, 255), "others white");
	portled_speed_rgb(NULL, 0, rgb);
	CHECK(same(rgb, 0, 0, 0), "no link dark");
	/* a speed's own colour (as the UniFi controller sets them) */
	CHECK(portled_speed_word("2g5") == 3 && portled_speed_index(2500) == 3 && portled_speed_word("2.5g") < 0
	      && portled_speed_index(500) < 0, "speed words and indexes");
	c.speed_set = 1 << 2;
	memcpy(c.speed[2], (const uint8_t[]){ 1, 2, 3 }, 3);
	portled_speed_rgb(&c, 1000, rgb);
	CHECK(same(rgb, 1, 2, 3), "1G in its own colour");
	portled_speed_rgb(&c, 100, rgb);
	CHECK(same(rgb, 255, 120, 0), "the others as they were");
	c.speed_set = 0;

	/* frames: off is dark; the order is green, red, blue; no link is dark */
	p[0] = (struct portled_port){ 1000, 1 };
	p[1] = (struct portled_port){ 0, 1 };
	p[2] = (struct portled_port){ 2500, 20 };
	portled_frame(&c, &l, p, 4, 0, f);
	CHECK(portled_ma(f, 4) == 0, "off: dark");
	c.on = 1;
	c.brightness = 100;
	portled_frame(&c, &l, p, 4, 0, f);
	CHECK(same(f, 0, 0, 255), "port 1 VLAN 1 blue, as G R B: %u %u %u", f[0], f[1], f[2]);
	CHECK(same(f + 3, 0, 0, 0) && same(f + 9, 0, 0, 0), "no link: dark");
	portled_vlan_rgb(&c, 20, rgb);
	CHECK(same(f + 6, rgb[1], rgb[0], rgb[2]), "port 3 in VLAN 20's colour");
	c.mode = PORTLED_SPEED;
	portled_frame(&c, &l, p, 4, 0, f);
	CHECK(same(f, 255, 0, 0) && same(f + 6, 255, 0, 120), "by speed: 1G green, 2.5G teal (G R B)");

	/* brightness, then the budget */
	c.brightness = 50;
	portled_frame(&c, &l, p, 4, 0, f);
	CHECK(same(f, 127, 0, 0), "half: %u", f[0]);
	c.brightness = 100;
	for (int i = 0; i < PORTLED_MAX; i++)
		p[i] = (struct portled_port){ 500, 1 };		/* white */
	c.budget_ma = 1000;
	portled_frame(&c, &l, p, PORTLED_MAX, 0, f);
	CHECK(portled_ma(f, PORTLED_MAX) == 600, "ten white LEDs: 600 mA, %u", portled_ma(f, PORTLED_MAX));
	c.budget_ma = 300;
	portled_frame(&c, &l, p, PORTLED_MAX, 0, f);
	CHECK(portled_ma(f, PORTLED_MAX) <= 300 && portled_ma(f, PORTLED_MAX) >= 290 && f[0] == f[29],
	      "held to 300 mA: %u", portled_ma(f, PORTLED_MAX));
	c.budget_ma = 0;
	portled_frame(&c, &l, p, PORTLED_MAX, 0, f);
	CHECK(portled_ma(f, PORTLED_MAX) == 0, "a budget of 0: dark");
	c.budget_ma = 300;

	/* breathing: from a fifth of the brightness to all of it, and back; locating doesn't */
	c.budget_ma = 2000;
	c.breathe = 1;
	p[0] = (struct portled_port){ 1000, 1 };	/* green */
	portled_frame(&c, &l, p, 1, 0, f);
	uint8_t dim = f[0];
	portled_frame(&c, &l, p, 1, PORTLED_BREATHE_MS / 2, f);
	uint8_t bright = f[0];
	portled_frame(&c, &l, p, 1, PORTLED_BREATHE_MS / 4, f);
	CHECK(dim == 51 && bright == 255 && f[0] > dim && f[0] < bright, "breathing: %u, %u, between %u", dim, bright, f[0]);
	portled_locate(&l, 1, 1000);
	portled_frame(&c, &l, p, 1, 0, f);
	CHECK(same(f, 255, 255, 255), "a locate flashes in full");
	portled_locate(&l, 1, 0);
	c.breathe = 0;

	/* locating: one port flashes white, then stops; all of them; no wrap trouble */
	memset(p, 0, sizeof(p));
	c.brightness = 100;
	c.budget_ma = 2000;
	portled_locate(&l, 2, 10000);
	portled_frame(&c, &l, p, 4, 9000, f);
	CHECK(same(f + 3, 255, 255, 255) && same(f, 0, 0, 0), "port 2 located: white, port 1 dark");
	portled_frame(&c, &l, p, 4, 9000 + PORTLED_LOCATE_MS, f);
	CHECK(same(f + 3, 0, 0, 0), "and off between");
	portled_frame(&c, &l, p, 4, 10000, f);
	CHECK(same(f + 3, 0, 0, 0), "until it's over");
	portled_locate(&l, 2, 0);
	portled_locate(&l, 0, 0xfffffff0u + 600);	/* across the wrap of the uptime */
	portled_frame(&c, &l, p, 4, 0xfffffff0u, f);
	CHECK(portled_ma(f, 4) == 240 || portled_ma(f, 4) == 0, "all located: four white (240 mA) or between");
	portled_frame(&c, &l, p, 4, 100, f);
	int lit = portled_ma(f, 4);
	portled_frame(&c, &l, p, 4, 100 + PORTLED_LOCATE_MS, f);
	CHECK(lit + (int)portled_ma(f, 4) == 240, "all flash, across the wrap");
	portled_locate(&l, 0, 0);
	portled_frame(&c, &l, p, 4, 100, f);
	CHECK(portled_ma(f, 4) == 0, "and stop");
	portled_locate(&l, PORTLED_MAX + 1, 5);
	CHECK(1, "a port past the last is ignored");

	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
