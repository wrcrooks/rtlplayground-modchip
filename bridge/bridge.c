/*
 * Mod-chip bridge firmware for the RTL8372/RTL8373.
 *
 * The 8051 does only what has to run on it:
 *  - initialise the switch, so that it forwards as an unmanaged switch even
 *    with no host attached;
 *  - keep the SerDes matched to the link speed and to inserted SFP modules;
 *  - move frames between the CPU port and the host, because the CPU port's
 *    buffers are reachable only through the 8051's DMA SFRs;
 *  - run the operations that need the 8051's private engines (PHY and
 *    SerDes access, the SFP I2C master, the SPI flash) on the host's behalf.
 *
 * Everything else (STP, LACP, IGMP, LLDP, the IP stack, the web UI, the
 * configuration) belongs to the host, normally the RP2040 on the mod chip,
 * which also reaches every switch register through the slave interface on
 * header T3. The link to the host is the UART on header T5; bridge_proto.h
 * defines the protocol and modchip/ARCHITECTURE.md describes the design.
 */

#include <8051.h>
#include <stdint.h>
#include <stdbool.h>

#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl837x_common.h"
#include "rtl837x_flash.h"
#include "rtl837x_pins.h"
#include "rtl837x_phy.h"
#include "rtl837x_port.h"
#include "rtl837x_trap.h"
#include "rtl837x_nic.h"
#include "rtl837x_leds.h"
#include "rtl837x_init.h"
#include "machine.h"
#include "phy.h"
#include "boot.h"
#include "sfp.h"
#include "version.h"
#include "bridge.h"
#include "bridge_proto.h"
#include "link.h"

extern __code const struct machine machine;
extern __xdata uint32_t flash_size;
extern __xdata struct flash_region_t flash_region;
extern __xdata uint16_t crc_value;
void crc16_bank1(__xdata uint8_t *v) __naked;

/* Globals the shared modules expect from the main program */
__xdata struct machine_runtime machine_detected;
__xdata struct uip_eth_addr uip_ethaddr;
__xdata char hostname[24];
__xdata uint8_t vlan_names[VLAN_NAMES_SIZE];
__xdata uint16_t vlan_ptr;
__xdata uint8_t flash_buf[FLASH_BUF_SIZE];
__xdata uint8_t rx_headers[16];
__xdata uint8_t tx_seq;
__code const uint8_t * __code const hex = "0123456789abcdef";

volatile __xdata uint32_t ticks;
volatile __xdata uint8_t sec_counter;
volatile __xdata uint16_t sleep_ticks;

__data __at(0x20) volatile uint8_t evflags;
volatile __bit __at(0x00) link_irq;
volatile __bit __at(0x01) rx_irq;
volatile __bit __at(0x02) tick_pending;
volatile __bit tx_buf_empty;

#define SFP_TICK_STEP 10
__xdata uint8_t sfp_tick_last;
__xdata uint8_t linkbits_last[4];
__xdata uint8_t linkbits_last_p89;

/*
 * Link receive ring, filled by the serial ISR. The host sends one request
 * at a time, and the largest (a TX_FRAME) encodes to under 1600 bytes.
 */
#define LINK_RX_SIZE 2048
#define LINK_RX_MASK (LINK_RX_SIZE - 1)
__xdata uint8_t link_rx[LINK_RX_SIZE];
volatile __xdata uint16_t link_rx_head;
__xdata uint16_t link_rx_tail;
volatile __xdata uint8_t link_rx_overflow;

/*
 * A decoded request, placed MSG_OFS bytes into an 8-byte aligned buffer:
 * a TX_FRAME's frame then starts 8 bytes in, and its NIC descriptor is
 * written over type, seq and flags in front of it. The NIC DMA reads up
 * to 15 bytes past the frame.
 */
#define MSG_OFS 5
__xdata uint8_t msg_raw[MSG_OFS + LINK_MSG_MAX + 16 + 7];
__xdata uint8_t *msg_base;
#define MSG link_msg

/* Received frames are copied here from the NIC, also 8-byte aligned */
__xdata uint8_t frame_raw[UIP_CONF_BUFFER_ALLOC_SIZE + 7];
__xdata uint8_t *frame_buf;

__xdata uint8_t link_seq;

/* Text printed by the shared modules, sent as LOG messages */
#define LOG_SIZE 128
__xdata uint8_t log_buf[LOG_SIZE];
__xdata uint8_t log_len;

__bit host_present;
__xdata uint8_t host_idle_s;
__xdata uint32_t uptime_s;
__xdata uint16_t rx_dropped;
__xdata uint16_t link_errors;


void isr_timer0(void) __interrupt(1)
{
}


void isr_timer2(void) __interrupt(5)
{
	ticks++;
	tick_pending = 1;
	if (sleep_ticks > 0)
		sleep_ticks--;
	sec_counter++;

	// Clear TF2 & EXF2 by software
	T2CON &= ~0xC0;
}


void isr_serial(void) __interrupt(4)
{
	if (RI == 1) {
		uint16_t next = (link_rx_head + 1) & LINK_RX_MASK;
		RI = 0;
		if (next == link_rx_tail) {
			link_rx_overflow = 1;
		} else {
			link_rx[link_rx_head] = SBUF;
			link_rx_head = next;
		}
	}
	if (TI == 1) {
		TI = 0;
		tx_buf_empty = 1;
	}
}


/* The register access helpers share the SFR data bytes with the main loop,
 * so the handlers only latch the events */
void isr_ext0(void) __interrupt(0)
{
	EX0 = 0;
	link_irq = 1;
}


void isr_ext1(void) __interrupt(2)
{
	EX1 = 0;
	rx_irq = 1;
}


void isr_ext2(void) __interrupt(8)
{
	EXIF &= 0xef;
}


void isr_ext3(void) __interrupt(9)
{
	EXIF &= 0xdf;
}


void uart_putc(uint8_t c)
{
	while (!tx_buf_empty)
		;
	tx_buf_empty = 0;
	SBUF = c;
}


static void log_flush(void)
{
	if (!log_len)
		return;
	link_begin(MSG_LOG, link_seq++);
	link_put_buf(log_buf, log_len);
	link_end();
	log_len = 0;
}


/*
 * The shared modules print through here. A line goes out as a LOG message
 * unless a message is being sent, in which case it waits for the main loop.
 */
void write_char(char c)
{
	if (c == '\r')
		return;
	if (log_len < LOG_SIZE)
		log_buf[log_len++] = c;
	if ((c == '\n' || log_len == LOG_SIZE) && !link_busy)
		log_flush();
}


static void put_hello(void)
{
	uint8_t flags = 0;

	link_put(BRIDGE_PROTO_VERSION);
	reg_read_m(RTL837X_REG_CHIP_ID);
	link_put(sfr_data[3]); link_put(sfr_data[2]); link_put(sfr_data[1]); link_put(sfr_data[0]);
	link_put_buf(uip_ethaddr.addr, 6);
	link_put32(flash_size);
	link_put32(IMAGE_SIZE);
	link_put32(FIRMWARE_UPLOAD_START);
	link_put(CPU_PORT);
	link_put(machine.min_port);
	link_put(machine.max_port);
	link_put(machine.n_sfp);
	if (machine_detected.isRTL8373)
		flags |= HELLO_RTL8373;
	if (machine_detected.isN)
		flags |= HELLO_N_VARIANT;
#ifdef LAYOUT_COMPACT
	flags |= HELLO_LAYOUT_COMPACT;
#endif
	link_put(flags);
	for (uint8_t i = 0; i < sizeof(machine.machine_name); i++)
		link_put(machine.machine_name[i]);
	for (__code const char *p = VERSION_SW; *p; p++)
		link_put(*p);
}


static void send_link_state(void)
{
	link_begin(MSG_LINK, link_seq++);
	reg_read_m(RTL837X_REG_LINKS_STS);
	link_put(sfr_data[3]); link_put(sfr_data[2]); link_put(sfr_data[1]); link_put(sfr_data[0]);
	reg_read_m(RTL837X_REG_LINKS);
	link_put(sfr_data[3]); link_put(sfr_data[2]); link_put(sfr_data[1]); link_put(sfr_data[0]);
	reg_read_m(RTL837X_REG_LINKS_89);
	link_put(sfr_data[3]); link_put(sfr_data[2]); link_put(sfr_data[1]); link_put(sfr_data[0]);
	link_end();
}


static void send_sfp_state(void)
{
	link_begin(MSG_SFP, link_seq++);
	link_put(sfp_pins_last);
	link_end();
}


/* A reply carrying only a status */
static void reply(uint8_t type, uint8_t seq, uint8_t status)
{
	link_begin(type | REPLY, seq);
	link_put(status);
	link_end();
}


/* The SFR register engine leaves its result in sfr_data, most significant
 * byte first; the protocol is little endian */
static void reply_sfr32(uint8_t type, uint8_t seq)
{
	link_begin(type | REPLY, seq);
	link_put(ST_OK);
	link_put(sfr_data[3]); link_put(sfr_data[2]); link_put(sfr_data[1]); link_put(sfr_data[0]);
	link_end();
}


/* PHY and SerDes reads leave a 16-bit result in SFR A6/A7 */
static void reply_sfr16(uint8_t type, uint8_t seq)
{
	uint8_t hi = SFR_DATA_8;
	uint8_t lo = SFR_DATA_0;

	link_begin(type | REPLY, seq);
	link_put(ST_OK);
	link_put(lo);
	link_put(hi);
	link_end();
}


static uint16_t get16(__xdata uint8_t *p)
{
	return p[0] | ((uint16_t)p[1] << 8);
}


static uint32_t get32(__xdata uint8_t *p)
{
	return get16(p) | ((uint32_t)get16(p + 2) << 16);
}


static void tx_frame(uint8_t seq, uint16_t frame_len)
{
	__xdata uint8_t *desc = msg_base;
	uint8_t flags = MSG[2];

	if (frame_len < 14 || frame_len > UIP_CONF_BUFFER_SIZE) {
		reply(REQ_TX_FRAME, seq, ST_BAD_LENGTH);
		return;
	}

	// NIC TX descriptor, see doc/CpuPort.md. It overwrites type, seq and
	// flags, which lie right in front of the frame.
	desc[0] = tx_seq++;
	desc[1] = flags;
	desc[2] = 0; desc[3] = 0;
	desc[4] = frame_len; desc[5] = frame_len >> 8;
	desc[6] = 0; desc[7] = 0;

	reg_read_m(RTL837X_REG_CPU_TX_CURR_PKT);
	nic_tx_packet(desc, frame_len, ((uint16_t)sfr_data[2] << 8) | sfr_data[3]);
	reg_read_m(RTL837X_REG_NIC_TX_CURR_PKT);
	REG_SET(RTL837X_REG_NIC_TXCMD, 1);

	reply(REQ_TX_FRAME, seq, ST_OK);
}


static bool flash_range_ok(uint32_t addr, uint16_t len)
{
	return addr >= FIRMWARE_UPLOAD_START && addr + len <= flash_size && !(addr & 3);
}


static void flash_writes(bool on)
{
	// Erasing and programming need the flash in single-IO mode
	flash_init(on ? 0 : (machine.flash_sio ? 0 : 1));
}


static void handle_request(uint16_t len)
{
	uint8_t type = MSG[0];
	uint8_t seq = MSG[1];
	__xdata uint8_t *p = MSG + 2;
	uint16_t n = len - 2;	/* payload length */
	uint32_t addr;

	host_present = 1;
	host_idle_s = 0;

	switch (type) {
	case REQ_HELLO:
		link_begin(REQ_HELLO | REPLY, seq);
		link_put(ST_OK);
		put_hello();
		link_end();
		send_link_state();
		send_sfp_state();
		return;

	case REQ_PING:
		link_begin(REQ_PING | REPLY, seq);
		link_put(ST_OK);
		link_put32(uptime_s);
		link_put16(rx_dropped);
		link_put16(link_errors);
		link_end();
		return;

	case REQ_TX_FRAME:
		if (n < 1)
			break;
		tx_frame(seq, n - 1);
		return;

	case REQ_REG_READ:
		if (n != 2)
			break;
		reg_read_m(get16(p));
		reply_sfr32(type, seq);
		return;

	case REQ_REG_WRITE:
		if (n != 6)
			break;
		sfr_data[3] = p[2]; sfr_data[2] = p[3]; sfr_data[1] = p[4]; sfr_data[0] = p[5];
		reg_write_m(get16(p));
		reply(type, seq, ST_OK);
		return;

	case REQ_PHY_READ:
		if (n != 4)
			break;
		if (p[0] > 15) {
			reply(type, seq, ST_BAD_ARGUMENT);
			return;
		}
		phy_read(p[0], p[1], get16(p + 2));
		reply_sfr16(type, seq);
		return;

	case REQ_PHY_WRITE:
		if (n != 6)
			break;
		if (p[0] > 15) {
			reply(type, seq, ST_BAD_ARGUMENT);
			return;
		}
		phy_write(p[0], p[1], get16(p + 2), get16(p + 4));
		reply(type, seq, ST_OK);
		return;

	case REQ_SDS_READ:
		if (n != 3)
			break;
		if (p[0] > 1 || p[1] > 127) {
			reply(type, seq, ST_BAD_ARGUMENT);
			return;
		}
		sds_read(p[0], p[1], p[2]);
		reply_sfr16(type, seq);
		return;

	case REQ_SDS_WRITE:
		if (n != 5)
			break;
		if (p[0] > 1 || p[1] > 127) {
			reply(type, seq, ST_BAD_ARGUMENT);
			return;
		}
		sds_write_v(p[0], p[1], p[2], get16(p + 3));
		reply(type, seq, ST_OK);
		return;

	case REQ_SFP_READ:
		if (n != 3)
			break;
		if (p[0] >= machine.n_sfp || !p[2] || p[2] > 16) {
			reply(type, seq, ST_BAD_ARGUMENT);
			return;
		}
		if (!sfp_read_block(p[0], p[1], p[2])) {
			reply(type, seq, ST_FAILED);
			return;
		}
		link_begin(type | REPLY, seq);
		link_put(ST_OK);
		link_put_buf(sfp_buf, p[2]);
		link_end();
		return;

	case REQ_FLASH_READ:
		if (n != 6)
			break;
		addr = get32(p);
		n = get16(p + 4);
		if (!n || n > FLASH_BUF_SIZE || addr + n > flash_size || (addr & 3)) {
			reply(type, seq, ST_BAD_ARGUMENT);
			return;
		}
		flash_region.addr = addr;
		flash_region.len = n;
		flash_read_bulk(flash_buf);
		link_begin(type | REPLY, seq);
		link_put(ST_OK);
		link_put_buf(flash_buf, n);
		link_end();
		return;

	case REQ_FLASH_ERASE:
		if (n != 4)
			break;
		addr = get32(p);
		if (!flash_range_ok(addr, FLASH_SECTOR_SIZE) || (addr & (FLASH_SECTOR_SIZE - 1))) {
			reply(type, seq, ST_BAD_ARGUMENT);
			return;
		}
		flash_writes(true);
		flash_region.addr = addr;
		flash_sector_erase();
		flash_writes(false);
		reply(type, seq, ST_OK);
		return;

	case REQ_FLASH_WRITE:
		if (n < 5 || n > 4 + FLASH_PAGE_SIZE)
			break;
		addr = get32(p);
		n -= 4;
		// Within one page: the controller programs 4 bytes per command
		// and a page program does not carry over into the next page
		if (!flash_range_ok(addr, n) || (addr & (FLASH_PAGE_SIZE - 1)) + n > FLASH_PAGE_SIZE) {
			reply(type, seq, ST_BAD_ARGUMENT);
			return;
		}
		memcpy(flash_buf, p + 4, n);
		flash_writes(true);
		flash_region.addr = addr;
		flash_region.len = n;
		flash_write_bytes(flash_buf);
		flash_writes(false);
		reply(type, seq, ST_OK);
		return;

	case REQ_SET_LED:
		if (n != 1)
			break;
		if (p[0] > SYS_LED_ON) {
			reply(type, seq, ST_BAD_ARGUMENT);
			return;
		}
		set_sys_led_state(p[0]);
		reply(type, seq, ST_OK);
		return;

	case REQ_REBOOT:
		reply(type, seq, ST_OK);
		while (!tx_buf_empty)
			;
		delay(2);
		reset_chip();
		return;

	default:
		reply(type, seq, ST_UNKNOWN);
		return;
	}
	reply(type, seq, ST_BAD_LENGTH);
}


/* Hand ring space back to the serial ISR, which reads the tail */
static void link_rx_release(uint16_t tail)
{
	ES = 0;
	link_rx_tail = tail;
	ES = 1;
}


/* Decode what the serial ISR has queued, up to and including one message */
static void link_poll(void)
{
	uint16_t head, tail = link_rx_tail;

	if (link_rx_overflow) {
		link_rx_overflow = 0;
		link_rx_lost();
	}

	ES = 0;
	head = link_rx_head;
	ES = 1;

	while (tail != head) {
		uint8_t r = link_rx_byte(link_rx[tail]);
		tail = (tail + 1) & LINK_RX_MASK;

		if (r == LINK_RX_NONE)
			continue;
		link_rx_release(tail);
		if (r == LINK_RX_MSG)
			handle_request(link_msg_len);
		else
			link_errors++;
		return;
	}
	link_rx_release(tail);
}


/* Pass frames from the CPU port on to the host, or drop them without one */
#define RX_BUDGET 4

static void bridge_rx(void)
{
	uint8_t budget = RX_BUDGET;

	while (budget--) {
		reg_read(RTL837X_REG_NIC_RX_BUFF_DATA);
		if (!SFR_DATA_U16)
			break;
		reg_read(RTL837X_REG_CPU_RX_CURR_PKT);
		uint16_t ring_ptr = SFR_DATA_U16;
		ring_ptr <<= 3;
		if (!nic_rx_header(ring_ptr) || !nic_rx_packet(frame_buf, ring_ptr + 8)) {
			REG_SET(RTL837X_REG_NIC_RXCMD, 1);
			rx_dropped++;
			return;
		}
		REG_SET(RTL837X_REG_NIC_RXCMD, 1);

		if (!host_present) {
			rx_dropped++;
			continue;
		}
		link_begin(MSG_RX_FRAME, link_seq++);
		link_put_buf(rx_headers, 8);
		link_put_buf(frame_buf, ((uint16_t)rx_headers[5] << 8) | rx_headers[4]);
		link_end();
	}
}


/*
 * As check_links() in rtlplayground.c: an RTL8221 PHY behind SerDes 0
 * needs the SerDes mode changed with its link speed. Changes are reported.
 */
static void check_links(void)
{
	reg_read_m(RTL837X_REG_LINKS_89);
	__xdata uint8_t linkbits_p89 = sfr_data[3];

	reg_read_m(RTL837X_REG_LINKS);
	if (!memcmp(sfr_data, linkbits_last, 4) && linkbits_p89 == linkbits_last_p89)
		return;

	linkbits_last_p89 = linkbits_p89;
	if (!machine_detected.isRTL8373 && machine.n_sfp != 2) {
		uint8_t p5 = sfr_data[2] >> 4;
		uint8_t p5_last = linkbits_last[2] >> 4;
		memcpy(linkbits_last, sfr_data, 4);
		if (!machine.n_10g && p5_last != p5) {
			if (p5 == 0x5)	// 2.5GBit Mode
				sds_config(0, SDS_HISGMII);
			else		// 1GBit and 100Mbit
				sds_config(0, SDS_SGMII);
		}
		if (machine.n_10g)
			sds_config(0, SDS_QXGMII);
		if (machine.n_10g == 2)
			sds_config(1, SDS_QXGMII);
	} else {
		memcpy(linkbits_last, sfr_data, 4);
	}
	if (host_present)
		send_link_state();
}


static void handle_second(void)
{
	uptime_s++;
	link_irq = 1;	/* also poll, in case a link interrupt was missed */
	if (host_present && ++host_idle_s > HOST_TIMEOUT_S)
		host_present = 0;
	if (!host_present) {
		link_begin(MSG_BEACON, link_seq++);
		put_hello();
		link_end();
	}
}


static void handle_tick(void)
{
	if (sec_counter >= SYS_TICK_HZ) {
		sec_counter -= SYS_TICK_HZ;
		handle_second();
	}
	if (TICKS_DUE(sfp_tick_last, SFP_TICK_STEP)) {
		uint8_t pins = sfp_pins_last;

		sfp_tick_last += SFP_TICK_STEP;
		handle_sfp();
		if (host_present && pins != sfp_pins_last)
			send_sfp_state();
	}
}


static void idle(void)
{
	if (!evflags && link_rx_tail == link_rx_head)
		PCON |= 1;
	if (tick_pending) {
		tick_pending = 0;
		handle_tick();
	}
	if (link_irq) {
		link_irq = 0;
		REG_SET(RTL837X_ISR_INT_PORT_LINK_CHG, 0x3ff);
		EX0 = 1;
		check_links();
	}
	if (rx_irq) {
		rx_irq = 0;
		bridge_rx();
		REG_SET(RTL837X_NIC_INT_STS, NIC_INT_RXIS);
		reg_read(RTL837X_REG_NIC_RX_BUFF_DATA);
		if (SFR_DATA_U16)
			rx_irq = 1;
		else
			EX1 = 1;
	}
	link_poll();
	log_flush();
}


/* UART0 at the link rate, Timer 1 as baud rate generator */
static void setup_serial_timer1(void)
{
	TMOD &= 0x0F;
	TMOD |= 0x20;	// Timer1: Mode2: Timer, 8-bit with auto-reload
	CKCON |= 0x10;	// Timer 1 uses clk/4
	PCON |= 0x80;	// SMOD0 = 1: double the baud rate
	SCON = 0x50;	// Mode 1: ASYNC 8N1, receive enabled
	TH1 = LINK_TH1;
	TCON |= 0x40;	// Start timer 1

	ET1 = 0;
	TI = 0;
	RI = 0;
	tx_buf_empty = 1;
	ES = 1;
}


static void setup_external_irqs(void)
{
	REG_SET(RTL837X_ISR_SW_INT_MODE, 0x42);
	REG_SET(RTL837X_ISR_INT_PORT_LINK_CHG, 0x3ff);
	REG_SET(RTL837X_IMR_INT_PORT_LINK_STS_CHG, 0x3ff);

	link_irq = 1;
	IT0 = 1;	// External IRQ 0 on falling edge (link change)
	EX0 = 1;

	rx_irq = 1;
	EX1 = 1;	// External IRQ 1: the NIC has received a packet
	EX2 = 1;
	EX3 = 1;
	PX3 = 1;
}


/* Install an image the host staged at FIRMWARE_UPLOAD_START, as
 * check_and_flash_update_image() in rtlplayground.c */
static void check_and_flash_update_image(void)
{
	flash_read_jedecid();	// Also sets flash_size

	if (flash_size < FLASH_NEEDED_FOR_UPDATE) {
		print_string("Flash too small for updating\n");
		return;
	}

	flash_region.addr = FIRMWARE_UPLOAD_START;
	flash_region.len = 0x100;
	flash_read_bulk(flash_buf);
	if (flash_buf[0] != 0x00 || flash_buf[1] != 0x40)
		return;

	__xdata uint32_t dest = 0x0;
	__xdata uint32_t source = FIRMWARE_UPLOAD_START;
	__xdata uint16_t i;
	bool image_ok;
	print_string("Staged image found, checking it\n");
	flash_init(0);
	set_sys_led_state(SYS_LED_FAST);
	crc_value = 0x0000;
	for (i = 0; i < IMAGE_SIZE / FLASH_BUF_SIZE; i++) {
		flash_region.addr = source;
		flash_region.len = FLASH_BUF_SIZE;
		flash_read_bulk(flash_buf);
		for (uint16_t j = 0; j < FLASH_BUF_SIZE; j++)
			crc16_bank1(flash_buf + j);
		source += FLASH_BUF_SIZE;
	}
	// Printing sends a LOG message, which uses crc_value too
	image_ok = crc_value == 0xb001;
	if (image_ok) {
		print_string("Checksum OK, installing\n");
		source = FIRMWARE_UPLOAD_START;
		for (i = 0; i < IMAGE_COPY_LEN / FLASH_BUF_SIZE; i++) {
			flash_region.addr = source;
			flash_region.len = FLASH_BUF_SIZE;
			flash_read_bulk(flash_buf);
			if (i % 8 == 0) {
				flash_region.addr = dest;
				flash_sector_erase();
			}
			flash_region.addr = dest;
			flash_region.len = FLASH_BUF_SIZE;
			flash_write_bytes(flash_buf);
			dest += FLASH_BUF_SIZE;
			source += FLASH_BUF_SIZE;
		}
	} else {
		print_string("Checksum incorrect, erasing the staged image\n");
	}
	dest = FIRMWARE_UPLOAD_START;
	for (i = 0; i < IMAGE_SIZE / FLASH_SECTOR_SIZE; i++) {
		flash_region.addr = dest;
		flash_sector_erase();
		dest += FLASH_SECTOR_SIZE;
	}
	if (image_ok) {
		print_string("Done, resetting\n");
		log_flush();
		delay(20);
		reset_chip();
	}
	flash_init(machine.flash_sio ? 0 : 1);
}


static void read_mac(void)
{
	uip_ethaddr.addr[0] = 0xff;
	if (machine.mac_flash_offset) {
		flash_region.addr = machine.mac_flash_offset;
		flash_region.len = FLASH_BUF_SIZE;
		flash_read_bulk(flash_buf);
		// Only a globally administered unicast address
		if (flash_buf[0] != 0xff && !(flash_buf[0] & 0x03) && (flash_buf[0] | flash_buf[1] | flash_buf[2]))
			memcpy(uip_ethaddr.addr, flash_buf, 6);
	}
	if (uip_ethaddr.addr[0] == 0xff) {
		// None in flash: the same locally administered address RTLPlayground derives
		reg_read_m(RTL837X_REG_CHIP_UUID);
		uip_ethaddr.addr[0] = 0x06;
		uip_ethaddr.addr[3] = sfr_data[0] ^ sfr_data[3];
		uip_ethaddr.addr[4] = sfr_data[1] ^ sfr_data[3];
		uip_ethaddr.addr[5] = sfr_data[2] ^ sfr_data[3];
		reg_read_m(RTL837X_REG_CHIP_LOT_NO);
		uip_ethaddr.addr[1] = sfr_data[0] ^ sfr_data[2];
		uip_ethaddr.addr[2] = sfr_data[1] ^ sfr_data[3];
	}
}


void main(void)
{
	ticks = 0;
	sfp_tick_last = 0;
	link_rx_head = link_rx_tail = 0;
	link_rx_overflow = 0;
	log_len = 0;
	link_busy = 0;
	host_present = 0;
	msg_base = (__xdata uint8_t *)(((uint16_t)msg_raw + 7) & ~7);
	link_msg = msg_base + MSG_OFS;
	link_rx_reset();
	frame_buf = (__xdata uint8_t *)(((uint16_t)frame_raw + 7) & ~7);

	CKCON = 0;
	SFR_97 = 0;
	SFR_b9 = 0x00;
	SFR_ba = 0x80;
	IE = 0;
	EIE = 0;

	setup_clock();
	setup_timer2();
	setup_serial_timer1();
	setup_external_irqs();
	EA = 1;

	// Before any code in another bank is fetched, see issue #70
	flash_init(machine.flash_sio ? 0 : 1);

	sfp_pins_last = 0x33;	// no module, LOS, so an inserted module is set up
	linkbits_last[0] = linkbits_last[1] = linkbits_last[2] = linkbits_last[3] = linkbits_last_p89 = 0;

	reg_read_m(RTL837X_REG_CHIP_ID);
	machine_detected.isRTL8373 = sfr_data[1] == 0x73;
	machine_detected.isN = sfr_data[2] == 0x70;
	print_string("mod-chip bridge " VERSION_SW " on " MACHINE_MODEL "\n");
	if (machine.isRTL8373 != machine_detected.isRTL8373)
		print_string("INCORRECT MACHINE!\n");
	if (machine_detected.isRTL8373)
		rtl8224_enable();

	sfp_speed[0] = sfp_speed[1] = SFP_SPEED_AUTO;
	reg_bit_set(RTL837X_REG_RESET, RESET_NIC_BIT);
	do {
		reg_read(RTL837X_REG_RESET);
	} while (SFR_DATA_0 & (1 << RESET_NIC_BIT));

	read_mac();

	REG_SET(RTL837X_PIN_MUX_2, 0x0);
	init_smi();
	if (machine_detected.isRTL8373)
		rtl8224_wait_ready();
	rtl8373_revision();
	leds_setup();
	machine_custom_init();
	set_sys_led_state(SYS_LED_SLOW);
	if (machine_detected.isRTL8373)
		rtl8373_init();
	else
		rtl8372_init();
	delay(1000);

	check_and_flash_update_image();

	// The forwarding defaults RTLPlayground starts from: VLAN 1 on every
	// port, all ports and the CPU port reachable, learning on. The host
	// configures the rest.
	nic_setup();
	trap_init();
	vlan_setup();
	port_l2_setup();
	setup_i2c();
	setup_sfp_gpio();

	set_sys_led_state(SYS_LED_ON);
	print_string("bridge ready\n");

	while (1)
		idle();
}
