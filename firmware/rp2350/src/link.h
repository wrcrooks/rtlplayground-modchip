#ifndef _LINK_H_
#define _LINK_H_

/*
 * Framing of the bridge link, the host side of the 8051's link.c: each
 * message is  type, seq, payload..., CRC-16/MODBUS (low byte first), COBS
 * encoded and ended by 0x00. No hardware access here, so the host tests
 * compile it unchanged (test/test_link.c).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Largest decoded message: type, seq, a TX_FRAME's flags and 1556-byte frame, CRC */
#define LINK_MSG_MAX		(3 + 1556 + 2)
/* Largest encoded message: one COBS code byte per 254 data bytes, the delimiter */
#define LINK_ENC_MAX		(LINK_MSG_MAX + LINK_MSG_MAX / 254 + 2)

#define LINK_RX_NONE		0	/* inside a message */
#define LINK_RX_MSG		1	/* a valid message: type, seq, payload in buf, len bytes */
#define LINK_RX_ERROR		2	/* a message was dropped: bad framing, CRC or length */

uint16_t link_crc16(uint16_t crc, const uint8_t *p, size_t n);

/* Encoder. Appends to out, which must hold LINK_ENC_MAX bytes. */
struct link_tx {
	uint8_t *out;
	size_t len;		/* bytes written to out so far */
	size_t code_at;		/* index of the open block's code byte */
	uint8_t run;		/* data bytes in the open block */
	uint16_t crc;
};

void link_tx_begin(struct link_tx *tx, uint8_t *out, uint8_t type, uint8_t seq);
void link_tx_put(struct link_tx *tx, uint8_t b);
void link_tx_put_buf(struct link_tx *tx, const uint8_t *p, size_t n);
size_t link_tx_end(struct link_tx *tx);	/* returns the encoded length, delimiter included */

size_t link_encode(uint8_t *out, uint8_t type, uint8_t seq, const uint8_t *payload, size_t n);

/* Decoder, fed one byte at a time. */
struct link_rx {
	uint8_t buf[LINK_MSG_MAX];
	uint16_t len;		/* after LINK_RX_MSG: type + seq + payload, CRC removed */
	uint8_t left;		/* data bytes left in the current block */
	bool zero_pending;	/* the block that ended was shorter than 254 bytes */
	bool bad;		/* drop the rest of this message */
	uint32_t errors;
};

void link_rx_reset(struct link_rx *rx);
int link_rx_byte(struct link_rx *rx, uint8_t b);

#endif
