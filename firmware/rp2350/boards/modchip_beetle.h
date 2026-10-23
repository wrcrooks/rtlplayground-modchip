/*
 * DFRobot Beetle RP2350 (DFR1188) wired to the switch as a stand-in for the
 * mod chip until the boards arrive: RP2350A, 2 MiB flash, USB-C. Same pins
 * as the mod chip for the bridge UART, the 3.3 V sense and the reset
 * driver; see docs/beetle-test-rig.md. Build with -DMODCHIP_BOARD=beetle.
 */
#ifndef _BOARDS_MODCHIP_BEETLE_H
#define _BOARDS_MODCHIP_BEETLE_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

#define RTLPLAYGROUND_MODCHIP
#define RTLPLAYGROUND_MODCHIP_BEETLE
#define PICO_RP2350A 1

// T5: UART0 to the 8051 bridge, pads D0 and D1
#define MODCHIP_BRIDGE_UART		0
#define MODCHIP_BRIDGE_TX_PIN		0
#define MODCHIP_BRIDGE_RX_PIN		1
#define MODCHIP_BRIDGE_BAUD		976562	/* 125 MHz / 128, see the bridge's link.c */

// T3: I2C0 on D4/D5 (GP10/GP11 aren't brought out); unused by the firmware so far
#define MODCHIP_T3_I2C			0
#define MODCHIP_T3_SDA_PIN		4
#define MODCHIP_T3_SCL_PIN		5

// Reset driver (transistor, 1 kOhm to TP1 at D3's anode) on A1; high pulls the switch's reset low
#define MODCHIP_RESET_PIN		27
// 100k/100k divider from the switch's 3.3 V (T3 pin 4) on A0
#define MODCHIP_SW_SENSE_PIN		26

#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN		25
#endif

// The console is USB serial; the bridge has UART0
#undef PICO_DEFAULT_UART

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080	1
pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (2 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES		(2 * 1024 * 1024)
#endif

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV		2
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED	1
#endif

#endif
