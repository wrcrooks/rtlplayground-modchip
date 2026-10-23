#ifndef _NET_H_
#define _NET_H_

#include <stdint.h>

void net_init(void);
void net_up(void);
void net_down(void);
void net_input(uint8_t *frame, uint16_t len);
void net_poll(void);		/* each main loop pass */
void net_udp_poll(void);	/* each tick */
void net_second(void);

void httpd_init(void);
void httpd_second(void);

#endif
