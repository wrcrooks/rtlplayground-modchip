#ifndef _BRIDGE_H_
#define _BRIDGE_H_

/*
 * Mod-chip bridge firmware: the 8051 initialises the switch, keeps the
 * SerDes and SFP slots working, and hands everything else to a host (the
 * RP2040 on the mod chip) over the UART. See modchip/ARCHITECTURE.md.
 */

#define CLOCK_HZ 125000000
#define CLOCK_DIV 0
#define SYS_TICK_HZ 200

/*
 * Link baud rate. Timer 1 divides F_SYS / 4 by 16 * (256 - TH1) with SMOD0
 * set, which gives these rates exactly at 125 MHz:
 *   TH1 0xff: 1953125   0xfe: 976562   0xfd: 651041   0xfc: 488281
 * The RP2040 UART reaches 1953125 and 976562 exactly from a 125 MHz
 * clk_peri (divisors 4 and 8). 115200 (TH1 0xef) is 0.27 % off.
 */
#ifndef LINK_TH1
#define LINK_TH1 0xfe
#endif

/* The host must send something at least this often, or the bridge stops
 * forwarding frames to it and goes back to announcing itself */
#define HOST_TIMEOUT_S 3

extern __xdata uint8_t sfr_data[4];

/* In hal.c */
void write_char(char c);
void setup_clock(void);
void setup_timer2(void);

#endif
