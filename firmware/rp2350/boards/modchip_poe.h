/*
 * Mod chip for the MokerLink POE-2G040210GSM: RP2350A with a W25Q16JV
 * (2 MiB), or an RP2354A with its 2 MiB in-package flash; the same build
 * runs on both. Pinout from hardware/poe/modchip-poe.kicad_sch: the same
 * GPIOs as the first board, on the switch's T7 (slave interface) and T9
 * (UART0), with no T8 header and no reset driver. Build with
 * -DMODCHIP_BOARD=poe.
 */
#ifndef _BOARDS_MODCHIP_POE_H
#define _BOARDS_MODCHIP_POE_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

#define RTLPLAYGROUND_MODCHIP
#define RTLPLAYGROUND_MODCHIP_POE
#define PICO_RP2350A 1

// T9: UART0 to the 8051 bridge, over four wires to J3
#define MODCHIP_BRIDGE_UART		0
#define MODCHIP_BRIDGE_TX_PIN		0
#define MODCHIP_BRIDGE_RX_PIN		1
#define MODCHIP_BRIDGE_BAUD		976562	/* 125 MHz / 128, see the bridge's link.c */

// T7: the RTL8372's slave interface, I2C address 0x5c (pins 1 SCL, 3 SDA)
#define MODCHIP_T3_I2C			1
#define MODCHIP_T3_SDA_PIN		10
#define MODCHIP_T3_SCL_PIN		11
// T7 pins 5 and 6
#define MODCHIP_T3_SPI_DO_PIN		9
#define MODCHIP_T3_SPI_CS_PIN		8

// No reset driver: MODCHIP_RESET_PIN stays undefined, so a hung bridge is
// reported, not reset
// Divider from the switch's 3.3 V on T7 pin 4 (ahead of the ideal diode)
#define MODCHIP_SW_SENSE_PIN		26

#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN		22
#endif

// There is no free UART; the console is RTT over the SWD pads (J5)
#undef PICO_DEFAULT_UART

// ABM8-272-T3, 12 MHz
#define PICO_XOSC_STARTUP_DELAY_MULTIPLIER 64

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
