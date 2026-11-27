#ifndef _PORTLED_CTL_H_
#define _PORTLED_CTL_H_

/* The port LED board in the firmware, see portled_ctl.c and portled.h */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void portled_boot(void);		/* after store_load() */
void portled_second(void);		/* the ports' links and VLANs */
void portled_poll(void);		/* each pass of the main loop: the frames */
void portled_unifi_locate(bool on);	/* the controller's locate: all of them flash */

/* The settings: false if refused; each is kept in the store */
bool portled_enable(bool on);
bool portled_set_pin(uint8_t pin);	/* 0xff: none */
bool portled_set_mode(uint8_t mode);
bool portled_set_brightness(uint8_t pct);
bool portled_set_budget(uint16_t ma);
bool portled_set_vlan_colour(uint16_t vid, const uint8_t rgb[3]);	/* NULL: the colour from the ID */
bool portled_set_speed_colour(int speed, const uint8_t rgb[3]);	/* portled_speed_words' index; NULL: the default */
bool portled_set_breathe(bool on);
bool portled_locate_port(uint8_t port, uint16_t seconds);		/* 0 s: stop */
size_t portled_status(char *buf, size_t max);

/* portled_pio.c: the data line, by a PIO state machine */
bool portled_hw_pin_ok(uint8_t pin);	/* a GPIO the board has free */
bool portled_hw_pin(uint8_t pin);	/* take it (0xff: let go); false: no state machine free */
void portled_hw_send(const uint8_t *grb, uint8_t n);

#endif
