#ifndef _PORTLED_H_
#define _PORTLED_H_

/*
 * The port LED board: one WS2812B per front-panel port, chained, LED 1 first.
 * Each shows its port's VLAN (the port VLAN ID) in that VLAN's colour, or the
 * link speed, and flashes white to locate a port, or all of them while the
 * UniFi controller locates the switch. A port with no link is dark.
 *
 * This half is plain C (the frame for a moment, from the ports' state), so the
 * host tests can run it; portled_plat.c sends the frames with a PIO state machine.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PORTLED_MAX		10	/* LEDs, front-panel ports 1..10 */
#define PORTLED_COLOURS		16	/* VLANs with a colour of their own */
#define PORTLED_SPEEDS		6	/* 10M, 100M, 1G, 2.5G, 5G, 10G */
#define PORTLED_BREATHE_MS	3000	/* dim, bright, dim */
#define PORTLED_LOCATE_MS	250	/* on, then off */
#define PORTLED_BUDGET_MAX	500	/* mA at 5 V: the board's MT3608 boost takes about 1 A from 3.3 V for it */

enum portled_mode { PORTLED_VLAN, PORTLED_SPEED };

struct portled_cfg {
	uint8_t on;			/* 0, 1 */
	uint8_t mode;			/* enum portled_mode */
	uint8_t brightness;		/* 1..100 % */
	uint8_t pin;			/* the data line's GPIO, 0xff: none */
	uint16_t budget_ma;		/* the most the LEDs may draw together */
	uint8_t breathe;		/* 0, 1: brightness rises and falls */
	uint8_t speed_set;		/* bit i: speed[i] is set, else the default */
	uint8_t speed[PORTLED_SPEEDS][3];
	struct {
		uint16_t vid;		/* 0: unused */
		uint8_t rgb[3];
		uint8_t pad;
	} colour[PORTLED_COLOURS];
};

struct portled_port {
	uint16_t mbps;			/* 0: no link */
	uint16_t pvid;
};

/* Defaults: off, VLAN colours, 30 %, no pin, 300 mA */
void portled_defaults(struct portled_cfg *c);
/* A VLAN's colour: its own, else one picked from its ID (neighbouring IDs differ a lot) */
void portled_vlan_rgb(const struct portled_cfg *c, uint16_t vid, uint8_t rgb[3]);
/* A link speed's: as set, else 10/100M amber, 1G green, 2.5G teal, 5G cyan, 10G blue, others white; 0: dark */
void portled_speed_rgb(const struct portled_cfg *c, uint16_t mbps, uint8_t rgb[3]);
/* The index in speed[] of 10, 100, 1000, 2500, 5000, 10000 Mb/s, of "10m", "100m", "1g", "2g5", "5g", "10g"; -1: none */
int portled_speed_index(uint16_t mbps);
int portled_speed_word(const char *w);
extern const char *const portled_speed_words[PORTLED_SPEEDS];
/* Set (rgb) or drop (NULL) a VLAN's colour; false: no room */
bool portled_set_colour(struct portled_cfg *c, uint16_t vid, const uint8_t rgb[3]);

/* Locating: one port (1..PORTLED_MAX) until uptime ms until_ms, or all (port 0); until 0 stops it */
struct portled_locate {
	uint32_t until[PORTLED_MAX + 1];	/* [0]: all; 0: not */
};
void portled_locate(struct portled_locate *l, uint8_t port, uint32_t until_ms);

/*
 * The frame at uptime ms now: n LEDs, 3 bytes each in the WS2812B's order (green,
 * red, blue), scaled by the brightness and then down to the current budget (20 mA
 * per colour at full). All dark while off.
 */
void portled_frame(const struct portled_cfg *c, const struct portled_locate *l, const struct portled_port *ports,
		   uint8_t n, uint32_t now, uint8_t *grb);
/* What the frame's LEDs draw, mA (for the budget, and the tests) */
uint32_t portled_ma(const uint8_t *grb, uint8_t n);

#endif
