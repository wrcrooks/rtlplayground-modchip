#ifndef _LINK_H_
#define _LINK_H_

/*
 * Framing of the bridge link: COBS-encoded messages, each ended by 0x00 and
 * carrying a CRC-16/MODBUS, see bridge_proto.h. Kept apart from bridge.c so
 * that the host tests compile it unchanged (test/test_bridge_link.c).
 */

#include <stdint.h>
#include <stdbool.h>

#include "uip/uipopt.h"

/* Largest decoded message: type, seq, a TX_FRAME's flags and frame, CRC */
#define LINK_MSG_MAX (3 + UIP_CONF_BUFFER_SIZE + 2)

/* Results of link_rx_byte() */
#define LINK_RX_NONE	0	/* inside a message */
#define LINK_RX_MSG	1	/* a valid message is in link_msg, link_msg_len long */
#define LINK_RX_ERROR	2	/* a message was dropped: bad framing, CRC or length */

/* Supplied by the user of the link: sends one byte */
void uart_putc(uint8_t c);

extern __bit link_busy;
extern __xdata uint8_t *link_msg;
extern __xdata uint16_t link_msg_len;

void link_begin(uint8_t type, uint8_t seq);
void link_put(uint8_t b);
void link_put16(uint16_t v);
void link_put32(uint32_t v);
void link_put_buf(__xdata uint8_t *p, uint16_t len);
void link_end(void);

void link_rx_reset(void);
void link_rx_lost(void);
uint8_t link_rx_byte(uint8_t b);

#endif
