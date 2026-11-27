/* The port LED board's data line: WS2812B frames from a PIO state machine, see portled_ctl.h */
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"

#include "portled_ctl.h"
#include "ws2812.pio.h"

static PIO pio;
static uint sm, offset;
static uint8_t pin_now = 0xff;

/* The pins the board uses already: never the LEDs' */
bool portled_hw_pin_ok(uint8_t pin)
{
	static const uint8_t used[] = {
		MODCHIP_BRIDGE_TX_PIN, MODCHIP_BRIDGE_RX_PIN, MODCHIP_T3_SDA_PIN, MODCHIP_T3_SCL_PIN,
		MODCHIP_SW_SENSE_PIN, PICO_DEFAULT_LED_PIN,
#ifdef MODCHIP_RESET_PIN		/* the PoE board has none */
		MODCHIP_RESET_PIN,
#endif
#ifdef MODCHIP_T3_SPI_DO_PIN
		MODCHIP_T3_SPI_DO_PIN, MODCHIP_T3_SPI_CS_PIN,
#endif
#ifdef MODCHIP_T8_GPIO46_PIN
		MODCHIP_T8_GPIO46_PIN, MODCHIP_T8_GPIO47_PIN, MODCHIP_T8_GPIO48_PIN, MODCHIP_T8_GPIO49_PIN,
#endif
	};

	if (pin >= NUM_BANK0_GPIOS)
		return false;
	for (unsigned i = 0; i < sizeof(used); i++)
		if (used[i] == pin)
			return false;
	return true;
}

bool portled_hw_pin(uint8_t pin)
{
	if (pin_now != 0xff) {
		pio_sm_set_enabled(pio, sm, false);
		pio_remove_program_and_unclaim_sm(&ws2812_program, pio, sm, offset);
		gpio_deinit(pin_now);
		pin_now = 0xff;
	}
	if (pin == 0xff || !portled_hw_pin_ok(pin))
		return pin == 0xff;
	if (!pio_claim_free_sm_and_add_program_for_gpio_range(&ws2812_program, &pio, &sm, &offset, pin, 1, true))
		return false;
	pio_gpio_init(pio, pin);
	pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, true);
	pio_sm_config c = ws2812_program_get_default_config(offset);
	sm_config_set_sideset_pins(&c, pin);
	sm_config_set_out_shift(&c, false, true, 24);	/* from the top, 24 bits a word */
	sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
	sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (800000.0f * (ws2812_T1 + ws2812_T2 + ws2812_T3)));
	pio_sm_init(pio, sm, offset, &c);
	pio_sm_set_enabled(pio, sm, true);
	pin_now = pin;
	return true;
}

void portled_hw_send(const uint8_t *grb, uint8_t n)
{
	if (pin_now == 0xff)
		return;
	/* 8 words of FIFO: the rest waits about 30 us an LED */
	for (uint8_t i = 0; i < n; i++)
		pio_sm_put_blocking(pio, sm, (uint32_t)grb[3 * i] << 24 | (uint32_t)grb[3 * i + 1] << 16 | (uint32_t)grb[3 * i + 2] << 8);
}
