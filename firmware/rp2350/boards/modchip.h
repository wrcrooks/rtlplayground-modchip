/*
 * RTLPlayground mod chip: RP2350A with a W25Q128JV (16 MiB), or an RP2354A
 * with its 2 MiB in-package flash (build with -DMODCHIP_RP2354=ON).
 * Pinout from hardware/modchip.kicad_sch.
 */
#ifndef _BOARDS_MODCHIP_H
#define _BOARDS_MODCHIP_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

#define RTLPLAYGROUND_MODCHIP
#define PICO_RP2350A 1

// T5: UART0 to the 8051 bridge
#define MODCHIP_BRIDGE_UART		0
#define MODCHIP_BRIDGE_TX_PIN		0
#define MODCHIP_BRIDGE_RX_PIN		1
#define MODCHIP_BRIDGE_BAUD		976562	/* 125 MHz / 128, see the bridge's link.c */

// T3: the RTL8372's slave interface, I2C address 0x5c
#define MODCHIP_T3_I2C			1
#define MODCHIP_T3_SDA_PIN		10
#define MODCHIP_T3_SCL_PIN		11
#define MODCHIP_T3_SPI_DO_PIN		9
#define MODCHIP_T3_SPI_CS_PIN		8

// T8: switch GPIO46-49
#define MODCHIP_T8_GPIO46_PIN		7
#define MODCHIP_T8_GPIO48_PIN		6
#define MODCHIP_T8_GPIO47_PIN		5
#define MODCHIP_T8_GPIO49_PIN		4

// 2N7002 to TP1, wired to D3's anode: high pulls the switch's reset low
#define MODCHIP_RESET_PIN		27
// Divider from the switch's 3.3 V (ahead of the ideal diode): high while the switch is powered
#define MODCHIP_SW_SENSE_PIN		26

#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN		22
#endif

// There is no free UART; the console is RTT over the SWD pads (J5)
#undef PICO_DEFAULT_UART

// ABM8-272-T3, 12 MHz
#define PICO_XOSC_STARTUP_DELAY_MULTIPLIER 64

#ifdef MODCHIP_RP2354
pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (2 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES		(2 * 1024 * 1024)
#endif
#else
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080	1
pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES		(16 * 1024 * 1024)
#endif
#endif

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV		2
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED	1
#endif

#endif
