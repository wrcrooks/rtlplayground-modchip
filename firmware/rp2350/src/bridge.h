#ifndef _BRIDGE_H_
#define _BRIDGE_H_

/*
 * Host side of the 8051 bridge link (protocol v1, RTLPlayground's
 * modchip/bridge/README.md). Requests are synchronous: one in flight, as the
 * protocol requires. Unsolicited messages that arrive meanwhile are queued
 * and handed to the bridge_on_*() handlers from bridge_dispatch(), never
 * from inside a request, so a handler may itself make requests.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_proto.h"

#define BRIDGE_TIMEOUT		(-1)

struct bridge_hello {
	uint8_t version;
	uint32_t chip_id;
	uint8_t mac[6];
	uint32_t flash_size;
	uint32_t image_size;
	uint32_t upload_start;
	uint8_t cpu_port, min_port, max_port, n_sfp;
	uint8_t flags;			/* HELLO_* */
	char machine[31];
	char version_sw[48];
};

struct bridge_stats {
	uint32_t requests;
	uint32_t timeouts;
	uint32_t link_errors;		/* frames we dropped: CRC, framing */
	uint32_t events_dropped;	/* unsolicited messages lost to a full queue */
	uint32_t rx_overruns;		/* bytes lost to a full UART ring */
	uint32_t resets;		/* reset pulses sent */
	uint32_t uptime_s;		/* from the last PING */
	uint16_t bridge_rx_dropped;	/* frames the 8051 dropped, from the last PING */
	uint16_t bridge_link_errors;	/* messages the 8051 dropped, from the last PING */
};

extern struct bridge_hello bridge_hello;
extern struct bridge_stats bridge_stats;

void bridge_init(void);
void bridge_deinit(void);
bool bridge_is_up(void);

/*
 * Send a request and wait for its reply. The payload is the concatenation
 * of p1 and p2 (either may be NULL). Returns the reply's status, ST_*, or
 * BRIDGE_TIMEOUT; on ST_OK the reply payload after the status byte is in
 * reply (up to rmax bytes) and its length in *rlen.
 */
int bridge_request(uint8_t type, const void *p1, size_t n1, const void *p2, size_t n2,
		   void *reply, size_t rmax, size_t *rlen);

/* Reads UART bytes and queues what arrives; safe anywhere outside an ISR */
void bridge_service(void);
/* Hands queued unsolicited messages to the handlers; main loop only */
void bridge_dispatch(void);
/* HELLO, PING and reset handling; call once a second from the main loop */
void bridge_second(void);

/* Handlers, defined by the application */
void bridge_on_up(void);			/* HELLO answered (again) */
void bridge_on_down(void);
void bridge_on_rx_frame(const uint8_t *desc, uint8_t *frame, uint16_t len);
void bridge_on_link(uint32_t links_sts, uint32_t links, uint32_t links_89);
void bridge_on_sfp(uint8_t sfp_pins);
void bridge_on_log(const char *text, size_t len);

#endif
