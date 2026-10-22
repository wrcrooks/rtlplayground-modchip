#ifndef _BRIDGE_PROTO_H_
#define _BRIDGE_PROTO_H_

/*
 * Bridge link protocol, version 1. Specified in modchip/ARCHITECTURE.md.
 *
 * Every message is  type, seq, payload..., CRC-16 (little endian)  encoded
 * with COBS and ended by a 0x00 byte. The CRC is CRC-16/MODBUS (reflected
 * polynomial 0xa001, initial value 0xffff) over type, seq and payload.
 * Multi-byte fields are little endian.
 *
 * The host sends one request at a time and waits for its reply, which
 * echoes the request's seq and has type request | 0x80 and a status byte
 * first. The bridge also sends unsolicited messages (0x40-0x4f).
 */

#define BRIDGE_PROTO_VERSION	1

/* Requests, host -> bridge */
#define REQ_HELLO		0x01	/* -> HELLO payload */
#define REQ_PING		0x02	/* -> uptime_s:4 rx_dropped:2 link_errors:2 */
#define REQ_TX_FRAME		0x10	/* flags:1 frame... */
#define REQ_REG_READ		0x20	/* reg:2 -> value:4 */
#define REQ_REG_WRITE		0x21	/* reg:2 value:4 */
#define REQ_PHY_READ		0x22	/* phy:1 dev:1 reg:2 -> value:2 */
#define REQ_PHY_WRITE		0x23	/* phy:1 dev:1 reg:2 value:2 */
#define REQ_SDS_READ		0x24	/* sds:1 page:1 reg:1 -> value:2 */
#define REQ_SDS_WRITE		0x25	/* sds:1 page:1 reg:1 value:2 */
#define REQ_SFP_READ		0x26	/* slot:1 offset:1 len:1 (1-16) -> data */
#define REQ_FLASH_READ		0x30	/* addr:4 len:2 (1-512) -> data */
#define REQ_FLASH_ERASE		0x31	/* addr:4, one 4 KiB sector */
#define REQ_FLASH_WRITE		0x32	/* addr:4 data (1-256 bytes) */
#define REQ_SET_LED		0x3e	/* state:1, SYS_LED_* */
#define REQ_REBOOT		0x3f	/* replies, then resets the chip */

#define REPLY			0x80

/* Unsolicited, bridge -> host */
#define MSG_BEACON		0x40	/* HELLO payload, sent each second without a host */
#define MSG_RX_FRAME		0x41	/* rx_desc:8 frame... */
#define MSG_LINK		0x42	/* links_sts:4 links:4 links_89:4 */
#define MSG_SFP			0x43	/* sfp_pins:1, as sfp_pins_last in sfp.c */
#define MSG_LOG			0x44	/* text */

/* Reply status */
#define ST_OK			0
#define ST_BAD_LENGTH		1
#define ST_BAD_ARGUMENT		2
#define ST_FAILED		3
#define ST_UNKNOWN		4

/* HELLO flags */
#define HELLO_RTL8373		0x01
#define HELLO_N_VARIANT		0x02
#define HELLO_LAYOUT_COMPACT	0x04

#endif
