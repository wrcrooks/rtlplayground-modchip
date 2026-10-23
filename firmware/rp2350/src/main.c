/*
 * RTLPlayground mod chip, RP2350 application firmware.
 *
 * Takes over what RTLPlayground's 8051 firmware did above the chip level -
 * the web UI and its JSON endpoints, the command line, IP (lwIP), DHCP,
 * DNS, NTP, syslog, STP, LACP, IGMP, LLDP and the configuration - and
 * reaches the switch through the 8051 bridge firmware on T5. One core, one
 * main loop, as on the 8051.
 */

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>

#include "pico/flash.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "lwip/timeouts.h"

#include "bridge.h"
#include "net.h"
#include "metrics.h"
#include "plat.h"

#define SYS_TICK_MS		5		/* RTLPlayground's SYS_TICK_HZ, 200 */
#define CONFIG_OFFSET		(PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

/* GP26 sees the switch's 3.3 V through 100k/100k: about 2048 counts when on */
#define SENSE_ON		1500		/* 1.2 V */
#define SENSE_OFF		1000		/* 0.8 V */

#define SYS_LED_OFF		0
#define SYS_LED_FAST		1
#define SYS_LED_SLOW		2
#define SYS_LED_ON		3

static repeating_timer_t tick_timer;
static uint8_t led_state = SYS_LED_SLOW;
static bool switch_powered;

/* ---- what plat.h asks of this half ---- */

void plat_console_putc(char c)
{
	putchar_raw(c);
}


void plat_delay_ms(uint32_t ms)
{
	absolute_time_t until = make_timeout_time_ms(ms);

	while (!time_reached(until))
		bridge_service();
}


uint32_t plat_random32(void)
{
	return get_rand_32();
}


uint32_t plat_uptime_s(void)
{
	return to_ms_since_boot(get_absolute_time()) / 1000;
}


uint32_t plat_ms(void)
{
	return to_ms_since_boot(get_absolute_time());
}


/* For the dashboard: main loop load, heap, the RP2350's own temperature */
static uint16_t load_pm;
static int16_t rp_temp;

void plat_sys_read(struct plat_sys *s)
{
	extern char end, __HeapLimit;
	struct mallinfo mi = mallinfo();
	uint32_t total = &__HeapLimit - &end;
	uint32_t used = (char *)sbrk(0) - &end;

	s->load_pm = load_pm;
	s->heap_total = total;
	s->heap_free = total - used + mi.fordblks;
	s->temp = rp_temp;
}


/* The sensor on ADC input 4: 0.706 V at 27 degC, -1.721 mV/degC (datasheet) */
static void read_rp_temp(void)
{
	adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
	uint16_t raw = adc_read();
	adc_select_input(MODCHIP_SW_SENSE_PIN - 26);
	float v = raw * 3.3f / 4096;
	rp_temp = (int16_t)((27 - (v - 0.706f) / 0.001721f) * 10);
}


void plat_sys_led(uint8_t state)
{
	led_state = state & 3;
	if (bridge_is_up())
		bridge_request(REQ_SET_LED, &led_state, 1, NULL, 0, NULL, 0, NULL);
}


void plat_reboot(void)
{
	printf("Rebooting the switch and the mod chip\n");
	if (bridge_is_up())
		bridge_request(REQ_REBOOT, NULL, 0, NULL, 0, NULL, 0, NULL);
	sleep_ms(50);
	watchdog_reboot(0, 0, 0);
	for (;;)
		tight_loop_contents();
}


void plat_config_read(uint32_t off, uint8_t *dst, uint32_t len)
{
	memcpy(dst, (const uint8_t *)(XIP_BASE + CONFIG_OFFSET + off), len);
}


static void config_program(void *src)
{
	flash_range_erase(CONFIG_OFFSET, FLASH_SECTOR_SIZE);
	flash_range_program(CONFIG_OFFSET, src, FLASH_SECTOR_SIZE);
}


bool plat_config_write(const uint8_t *src, uint32_t len)
{
	static uint8_t sector[FLASH_SECTOR_SIZE];

	if (len > sizeof(sector))
		return false;
	memset(sector, 0xff, sizeof(sector));
	memcpy(sector, src, len);
	if (!memcmp(sector, (const void *)(XIP_BASE + CONFIG_OFFSET), sizeof(sector)))
		return true;
	return flash_safe_execute(config_program, sector, 100) == PICO_OK;
}

/* ---- the bridge's events ---- */

void bridge_on_up(void)
{
	rtl_bridge_up();
	net_up();
}


void bridge_on_down(void)
{
	printf("bridge: link lost\n");
	net_down();
	rtl_bridge_down();
}


void bridge_on_rx_frame(const uint8_t *desc, uint8_t *frame, uint16_t len)
{
	uint16_t n = desc[4] | desc[5] << 8;

	net_input(frame, n && n < len ? n : len);
}


void bridge_on_link(uint32_t links_sts, uint32_t links, uint32_t links_89)
{
	uint8_t b[12];

	memcpy(b, &links_sts, 4);
	memcpy(b + 4, &links, 4);
	memcpy(b + 8, &links_89, 4);
	rtl_on_link(b);
}


void bridge_on_sfp(uint8_t sfp_pins)
{
	rtl_on_sfp(sfp_pins);
}


void bridge_on_log(const char *text, size_t len)
{
	static bool line_start = true;

	for (size_t i = 0; i < len; i++) {
		if (line_start)
			printf("[8051] ");
		putchar_raw(text[i]);
		line_start = text[i] == '\n';
	}
}

/* ---- main ---- */

static bool on_tick(repeating_timer_t *t)
{
	(void)t;
	ticks++;
	return true;
}


u32_t sys_now(void)
{
	return to_ms_since_boot(get_absolute_time());
}


static void led_update(void)
{
	bool on;

	switch (led_state) {
	case SYS_LED_FAST:
		on = (ticks / 20) & 1;
		break;
	case SYS_LED_SLOW:
		on = (ticks / 100) & 1;
		break;
	case SYS_LED_ON:
		on = true;
		break;
	default:
		on = false;
	}
	gpio_put(PICO_DEFAULT_LED_PIN, on);
}


/*
 * Every switch-facing pin stays isolated until the switch has power, so
 * nothing back-powers it through I/O (bench supply on J5, switch off).
 */
static void check_power(void)
{
	uint16_t v = adc_read();

	if (!switch_powered && v >= SENSE_ON) {
		switch_powered = true;
		printf("Switch powered, starting the bridge link\n");
		bridge_init();
	} else if (switch_powered && v < SENSE_OFF) {
		switch_powered = false;
		printf("Switch unpowered, releasing its pins\n");
		bridge_deinit();
	}
}


/*
 * mbedTLS's key generation and handshakes want more stack than the 4 KiB
 * scratch bank the SDK gives core 0. The main loop runs on a stack in RAM
 * instead, with MSPLIM at its bottom so an overflow faults rather than
 * overwrites the heap.
 */
#define MAIN_STACK_SIZE		(32 * 1024)
static uint64_t main_stack[MAIN_STACK_SIZE / 8];
void main_loop(void);

static void __attribute__((noreturn, noinline)) run_main_loop(void)
{
	uintptr_t bottom = (uintptr_t)main_stack, top = bottom + sizeof(main_stack);

	__asm volatile("msr msplim, %0\n\tmsr msp, %1\n\tisb\n\tb main_loop" : : "r"(bottom), "r"(top) : "memory");
	__builtin_unreachable();
}


int main(void)
{
	stdio_init_all();
	printf("\nRTLPlayground mod chip, firmware " MODCHIP_VERSION "\n");

	gpio_init(PICO_DEFAULT_LED_PIN);
	gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
	adc_init();
	adc_gpio_init(MODCHIP_SW_SENSE_PIN);
	adc_set_temp_sensor_enabled(true);
	adc_select_input(MODCHIP_SW_SENSE_PIN - 26);

	add_repeating_timer_ms(-SYS_TICK_MS, on_tick, NULL, &tick_timer);
	net_init();
	httpd_init();
	rtl_boot();

	run_main_loop();
}


void __attribute__((noreturn, noinline, used)) main_loop(void)
{
	uint32_t tick_last = ticks;
	uint32_t second_last = plat_uptime_s();
	uint64_t busy_us = 0;

	for (;;) {
		uint64_t pass_start = time_us_64();
		bool work;

		check_power();
		work = bridge_dispatch();
		sys_check_timeouts();
		net_poll();
		work |= httpd_poll();

		int c = getchar_timeout_us(0);
		if (c >= 0) {
			rtl_console_input(c);
			work = true;
		}
		rtl_poll();

		/* Catch up a few ticks at most, as RTLPlayground's timers do */
		uint32_t now = ticks;
		if (now - tick_last > 8)
			tick_last = now - 8;
		while (tick_last != now) {
			tick_last++;
			rtl_tick();
			net_udp_poll();
			work = true;
		}

		if (plat_uptime_s() != second_last) {
			second_last = plat_uptime_s();
			bridge_second();
			rtl_second();
			net_second();
			httpd_second();
			snmp_second();
			read_rp_temp();
			metrics_second();
			load_pm = busy_us > 1000000 ? 1000 : busy_us / 1000;
			busy_us = 0;
			work = true;
		}
		led_update();
		/* The share of time spent on passes that had something to do */
		if (work)
			busy_us += time_us_64() - pass_start;
	}
}
