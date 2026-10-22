/*
 * Bridge link framing, see link.h and bridge_proto.h.
 *
 * Sending encodes on the fly: COBS needs at most 254 bytes of look-ahead,
 * so only one block is buffered and a message never has to be assembled
 * in memory first. Receiving decodes into link_msg as bytes arrive.
 *
 * Both directions use crc_value through crc16_bank1(). A message is sent
 * from start to end without anything else being sent in between, but one
 * can arrive over many main loop passes, so the receiver checks its CRC
 * only once the message is complete.
 */

#include <stdint.h>
#include <stdbool.h>

#include "link.h"

extern __xdata uint16_t crc_value;
void crc16_bank1(__xdata uint8_t *v) __naked;

__bit link_busy;
__xdata uint8_t *link_msg;
__xdata uint16_t link_msg_len;

static __xdata uint8_t enc_blk[254];
static __xdata uint8_t enc_len;
static __xdata uint8_t crc_byte;

static __xdata uint8_t dec_rem;
static __bit dec_zero;
static __bit dec_bad;
static __bit dec_done;


static void cobs_flush(uint8_t code)
{
	uart_putc(code);
	for (uint8_t i = 0; i < enc_len; i++)
		uart_putc(enc_blk[i]);
	enc_len = 0;
}


static void cobs_put(uint8_t b)
{
	if (!b) {
		cobs_flush(enc_len + 1);
		return;
	}
	enc_blk[enc_len++] = b;
	if (enc_len == 254)
		cobs_flush(0xff);
}


void link_put(uint8_t b)
{
	crc_byte = b;
	crc16_bank1(&crc_byte);
	cobs_put(b);
}


void link_put16(uint16_t v)
{
	link_put(v);
	link_put(v >> 8);
}


void link_put32(uint32_t v)
{
	link_put16(v);
	link_put16(v >> 16);
}


void link_put_buf(__xdata uint8_t *p, uint16_t len)
{
	while (len--)
		link_put(*p++);
}


void link_begin(uint8_t type, uint8_t seq)
{
	link_busy = 1;
	crc_value = 0xffff;
	enc_len = 0;
	link_put(type);
	link_put(seq);
}


void link_end(void)
{
	uint16_t crc = crc_value;

	cobs_put(crc);
	cobs_put(crc >> 8);
	cobs_flush(enc_len + 1);
	uart_putc(0);
	link_busy = 0;
}


void link_rx_reset(void)
{
	link_msg_len = 0;
	dec_rem = 0;
	dec_zero = 0;
	dec_bad = 0;
	dec_done = 0;
}


/* Bytes were lost: drop the message they belonged to */
void link_rx_lost(void)
{
	dec_bad = 1;
}


static void dec_out(uint8_t b)
{
	if (link_msg_len < LINK_MSG_MAX)
		link_msg[link_msg_len++] = b;
	else
		dec_bad = 1;
}


static uint8_t link_rx_end(void)
{
	if (dec_bad || dec_rem || link_msg_len < 4)
		return (link_msg_len || dec_bad || dec_rem) ? LINK_RX_ERROR : LINK_RX_NONE;

	crc_value = 0xffff;
	for (uint16_t i = 0; i < link_msg_len; i++)
		crc16_bank1(link_msg + i);
	// CRC-16/MODBUS run over data and its little-endian CRC leaves zero
	if (crc_value)
		return LINK_RX_ERROR;
	link_msg_len -= 2;
	return LINK_RX_MSG;
}


/*
 * Takes the next received byte. A message stays in link_msg until the
 * byte after its delimiter arrives.
 */
uint8_t link_rx_byte(uint8_t b)
{
	if (dec_done)
		link_rx_reset();
	if (!b) {
		dec_done = 1;
		return link_rx_end();
	}
	if (!dec_rem) {
		if (dec_zero)
			dec_out(0);
		dec_rem = b - 1;
		dec_zero = b != 0xff;
	} else {
		dec_out(b);
		dec_rem--;
	}
	return LINK_RX_NONE;
}
