# Host tests for the RP2350 firmware: make -C firmware/rp2350/test
CC ?= cc
CFLAGS = -std=gnu11 -O1 -g -Wall -Wextra -I../src
BUILD = build
RTLP = ../rtlplayground
MACHINE = SWGT024_V2_0_UNMANAGED

# The RTLPlayground half, built as CMakeLists.txt builds it for the RP2350
RTLP_SRC = cmd_parser.c cmd_editor.c httpd/page_impl.c rtl837x_port.c rtl837x_stp.c \
	rtl837x_lacp.c rtl837x_igmp.c rtl837x_lldp.c rtl837x_bandwidth.c rtl837x_storm.c \
	rtl837x_cabletest.c rtl837x_phy.c rtl837x_leds.c rtl837x_leds_dump.c rtl837x_pins.c \
	rtl837x_trap.c sfp.c dns.c dns_cli.c ntp.c ntp_cli.c syslog.c machine/$(MACHINE).c
SIM_SRC = $(addprefix $(RTLP)/,$(RTLP_SRC)) ../src/rtl_env.c ../src/rtl_app.c ../src/rtl_udp.c \
	$(BUILD)/web_assets.c sim.c
SIM_FLAGS = -std=gnu11 -O1 -g -fno-strict-aliasing -include $(RTLP)/test/sdcc_shim.h \
	-I../src -I../stub -I$(BUILD) -I$(RTLP) -I$(RTLP)/uip -I$(RTLP)/httpd -I$(RTLP)/modchip/bridge \
	-DRTLP_HOST_TEST -DMACHINE_$(MACHINE) -DMACHINE_NAME=$(MACHINE) \
	-Ddhcp_start=rtlp_dhcp_start -Ddhcp_stop=rtlp_dhcp_stop \
	-w -Wl,--wrap=sfp_read_block

all: $(BUILD)/test_link $(BUILD)/sim
	$(BUILD)/test_link ../../micropython/bridge_vectors.txt
	python3 run_sim.py $(BUILD)/sim

$(BUILD)/test_link: test_link.c ../src/link.c ../src/link.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_link.c ../src/link.c

$(BUILD)/version.h:
	@mkdir -p $(BUILD)
	printf '#define VERSION_SW "sim"\n#define BUILD_DATE "sim"\n' > $@

$(BUILD)/web_assets.c: ../tools/embed_web.py $(RTLP)/config.txt
	@mkdir -p $(BUILD)
	python3 ../tools/embed_web.py $@ $(RTLP)/html $(RTLP)/config.txt

$(BUILD)/rtl_sfr.o: ../src/rtl_sfr.S
	@mkdir -p $(BUILD)
	$(CC) -c -o $@ $<

$(BUILD)/sim: $(SIM_SRC) $(BUILD)/rtl_sfr.o $(BUILD)/version.h ../src/*.h
	$(CC) $(SIM_FLAGS) -o $@ $(SIM_SRC) $(BUILD)/rtl_sfr.o

clean:
	rm -rf $(BUILD)

.PHONY: all clean
