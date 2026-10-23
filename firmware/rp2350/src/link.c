/*
 * Bridge link framing, see link.h and RTLPlayground's modchip/bridge/README.md
 */

#include "link.h"

uint16_t link_crc16(uint16_t crc, const uint8_t *p, size_t n)
{
	while (n--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++)
			crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
	}
	return crc;
}


static void cobs_put(struct link_tx *tx, uint8_t b)
{
	if (b) {
		tx->out[tx->len++] = b;
		tx->run++;
	}
	if (!b || tx->run == 254) {
		tx->out[tx->code_at] = tx->run + 1;
		tx->code_at = tx->len++;
		tx->run = 0;
	}
}


void link_tx_begin(struct link_tx *tx, uint8_t *out, uint8_t type, uint8_t seq)
{
	tx->out = out;
	tx->code_at = 0;
	tx->len = 1;
	tx->run = 0;
	tx->crc = 0xffff;
	link_tx_put(tx, type);
	link_tx_put(tx, seq);
}


void link_tx_put(struct link_tx *tx, uint8_t b)
{
	tx->crc = link_crc16(tx->crc, &b, 1);
	cobs_put(tx, b);
}


void link_tx_put_buf(struct link_tx *tx, const uint8_t *p, size_t n)
{
	while (n--)
		link_tx_put(tx, *p++);
}


size_t link_tx_end(struct link_tx *tx)
{
	uint16_t crc = tx->crc;

	cobs_put(tx, crc);
	cobs_put(tx, crc >> 8);
	tx->out[tx->code_at] = tx->run + 1;
	tx->out[tx->len++] = 0;
	return tx->len;
}


size_t link_encode(uint8_t *out, uint8_t type, uint8_t seq, const uint8_t *payload, size_t n)
{
	struct link_tx tx;

	link_tx_begin(&tx, out, type, seq);
	link_tx_put_buf(&tx, payload, n);
	return link_tx_end(&tx);
}


void link_rx_reset(struct link_rx *rx)
{
	rx->len = 0;
	rx->left = 0;
	rx->zero_pending = false;
	rx->bad = false;
}


static int rx_drop(struct link_rx *rx)
{
	rx->errors++;
	link_rx_reset(rx);
	return LINK_RX_ERROR;
}


int link_rx_byte(struct link_rx *rx, uint8_t b)
{
	if (!b) {
		/* An empty frame is a lone delimiter, line noise or a resync */
		if (!rx->len && !rx->left && !rx->zero_pending && !rx->bad)
			return LINK_RX_NONE;
		if (rx->bad || rx->left || rx->len < 4
		    || link_crc16(0xffff, rx->buf, rx->len))
			return rx_drop(rx);
		rx->len -= 2;
		rx->left = 0;
		rx->zero_pending = false;
		return LINK_RX_MSG;
	}
	if (rx->bad)
		return LINK_RX_NONE;
	if (!rx->left) {
		/* A code byte: the zero the previous block stood for comes first */
		if (rx->zero_pending) {
			if (rx->len >= LINK_MSG_MAX) {
				rx->bad = true;
				return LINK_RX_NONE;
			}
			rx->buf[rx->len++] = 0;
		}
		rx->left = b - 1;
		rx->zero_pending = b < 0xff;
		return LINK_RX_NONE;
	}
	if (rx->len >= LINK_MSG_MAX) {
		rx->bad = true;
		return LINK_RX_NONE;
	}
	rx->buf[rx->len++] = b;
	rx->left--;
	return LINK_RX_NONE;
}
