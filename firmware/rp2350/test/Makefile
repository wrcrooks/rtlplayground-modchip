# Host tests for the RP2350 firmware: make -C firmware/rp2350/test
CC ?= cc
CFLAGS = -std=gnu11 -O1 -g -Wall -Wextra -I../src
BUILD = build
RTLP = ../rtlplayground
MACHINE = SWGT024_V2_0_UNMANAGED

# The RTLPlayground half, built as CMakeLists.txt builds it for the RP2350
RTLP_SRC = cmd_parser.c cmd_editor.c httpd/page_impl.c httpd/page_poe.c rtl837x_port.c rtl837x_stp.c \
	rtl837x_lacp.c rtl837x_igmp.c rtl837x_lldp.c rtl837x_l2sec.c rtl837x_bandwidth.c rtl837x_storm.c \
	rtl837x_cabletest.c rtl837x_phy.c rtl837x_leds.c rtl837x_leds_dump.c rtl837x_pins.c \
	rtl837x_trap.c rtl837x_acl.c rtl837x_acl_cmd.c sfp.c dns.c dns_cli.c ntp.c ntp_cli.c syslog.c machine/$(MACHINE).c
SIM_SRC = $(addprefix $(RTLP)/,$(RTLP_SRC)) ../src/rtl_env.c ../src/rtl_app.c ../src/rtl_udp.c ../src/rtl_ext.c ../src/rtl_metrics.c ../src/t3_route.c \
	../src/rtl_poe.c ../src/dot1x_plat.c ../src/rtl_guard.c ../src/rtl_mcast.c ../src/mcast.c ../src/dot1x.c ../src/md5.c ../src/events.c ../src/store.c ../src/safety.c \
	$(BUILD)/web_assets.c sim.c
SIM_FLAGS = -std=gnu11 -O1 -g -fno-strict-aliasing -include $(RTLP)/test/sdcc_shim.h -include ../stub/rtlp_ext.h -DCMD_PARSER_EXT=rtl_cmd_ext -DRTLP_ACL \
	-I../src -I../stub -I$(BUILD) -I$(RTLP) -I$(RTLP)/uip -I$(RTLP)/httpd -I$(RTLP)/modchip/bridge \
	-DRTLP_HOST_TEST -DMACHINE_$(MACHINE) -DMACHINE_NAME=$(MACHINE) \
	-Ddhcp_start=rtlp_dhcp_start -Ddhcp_stop=rtlp_dhcp_stop \
	-w -Wl,--wrap=sfp_read_block -Wl,--wrap=handle_sfp

# test_tls needs mbedTLS's sources: the Pico SDK's copy, if PICO_SDK_PATH is set
# and test_snmp lwIP's, from the same place
MBEDTLS = $(if $(PICO_SDK_PATH),$(PICO_SDK_PATH)/lib/mbedtls)
LWIP = $(if $(PICO_SDK_PATH),$(PICO_SDK_PATH)/lib/lwip)
SDK_TESTS = $(if $(PICO_SDK_PATH),$(BUILD)/test_usm $(BUILD)/test_tls $(BUILD)/test_snmp $(BUILD)/test_httpd $(BUILD)/test_httpd_tls)

all: $(BUILD)/test_link $(BUILD)/test_dot1x $(BUILD)/test_safety $(BUILD)/test_mcast $(BUILD)/sim $(BUILD)/sim-poe $(BUILD)/test_metrics $(BUILD)/test_upload $(SDK_TESTS)
	$(BUILD)/test_link ../../micropython/bridge_vectors.txt
	$(BUILD)/test_dot1x
	$(BUILD)/test_safety
	$(BUILD)/test_mcast
	python3 run_config_words.py $(BUILD)/test_safety
	$(BUILD)/test_upload
	python3 run_sim.py $(BUILD)/sim
	python3 run_sim.py $(BUILD)/sim-poe
	python3 run_metrics.py $(BUILD)/test_metrics $(BUILD)
	node check_dashboard.js ../web/dashboard.html $(BUILD)/history.bin $(BUILD)/metrics.json
	python3 -c "import sys; sys.path.insert(0, '../tools'); import embed_web; print(embed_web.SYSTEM_HTTPS + embed_web.DOT1X)" > $(BUILD)/system_https.html
	node check_conf_merge.js $(RTLP)/html/app.js $(BUILD)/system_https.html
ifneq ($(PICO_SDK_PATH),)
	$(BUILD)/test_tls
	$(BUILD)/test_usm
	python3 run_snmp.py $(BUILD)/test_snmp
	$(BUILD)/test_httpd
	$(BUILD)/test_httpd_tls
else
	@echo "SKIP  test_tls, test_snmp, test_httpd: set PICO_SDK_PATH for mbedTLS's and lwIP's sources"
endif

# mbedTLS as the firmware configures it, plus what the tests need
MBED_INC = -I. -I$(MBEDTLS)/include -I$(MBEDTLS)/library '-DMBEDTLS_CONFIG_FILE="mbedtls_test_config.h"'
$(BUILD)/libmbed.a: mbedtls_test_config.h ../src/mbedtls_config.h
	@mkdir -p $(BUILD)/mbed
	cd $(BUILD)/mbed && $(CC) -std=gnu11 -O2 -w -c -I../.. $(patsubst -I.,,$(MBED_INC)) $(wildcard $(MBEDTLS)/library/*.c)
	rm -f $@ && ar rcs $@ $(BUILD)/mbed/*.o

$(BUILD)/test_usm: test_usm.c ../src/usm.c ../src/usm.h $(BUILD)/libmbed.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined $(MBED_INC) -o $@ test_usm.c $(BUILD)/libmbed.a

SNMP_SRC = asn1 core mib2 mib2_icmp mib2_interfaces mib2_ip mib2_snmp mib2_system mib2_tcp mib2_udp \
	msg pbuf_stream scalar table traps
LWIP_SRC = $(wildcard $(LWIP)/src/core/*.c) $(wildcard $(LWIP)/src/core/ipv4/*.c) $(LWIP)/src/netif/ethernet.c \
	$(patsubst %,$(LWIP)/src/apps/snmp/snmp_%.c,$(SNMP_SRC))

$(BUILD)/test_snmp: test_snmp.c $(BUILD)/libmbed.a ../src/usm.c ../src/usm.h ../src/snmp.c ../src/snmp_mib.c ../src/snmp_modchip.h ../src/plat.h ../src/lwipopts.h lwip_host/lwipopts.h
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O1 -g -w -Ilwip_host -I../src -I$(LWIP)/src/include -I$(RTLP)/modchip/bridge \
		$(MBED_INC) '-DMODCHIP_VERSION="sim"' -o $@ test_snmp.c ../src/snmp.c ../src/snmp_mib.c ../src/usm.c ../src/events.c ../src/store.c \
		$(LWIP_SRC) $(BUILD)/libmbed.a

$(BUILD)/test_metrics: test_metrics.c ../src/metrics.c ../src/metrics.h ../src/plat.h ../src/bridge.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(RTLP)/modchip/bridge -o $@ test_metrics.c ../src/metrics.c

$(BUILD)/test_safety: test_safety.c ../src/events.c ../src/store.c ../src/safety.c ../src/events.h ../src/store.h ../src/safety.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ test_safety.c ../src/events.c ../src/store.c ../src/safety.c

$(BUILD)/test_mcast: test_mcast.c ../src/mcast.c ../src/mcast.h ../src/events.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ test_mcast.c ../src/mcast.c ../src/events.c

$(BUILD)/test_upload: test_upload.c ../src/upload.c ../src/upload.h ../src/link.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_upload.c ../src/upload.c ../src/link.c

LWIP_CORE = $(wildcard $(LWIP)/src/core/*.c) $(wildcard $(LWIP)/src/core/ipv4/*.c) $(LWIP)/src/netif/ethernet.c

HTTPD_SRC = ../src/upload.c ../src/link.c ../src/events.c ../src/store.c ../src/safety.c ../src/extras.c

$(BUILD)/test_httpd: test_httpd.c $(HTTPD_SRC) ../src/httpd.c ../src/upload.c ../src/upload.h ../src/lwipopts.h lwip_httpd/lwipopts.h
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O1 -g -w -Ilwip_httpd -I../src -I$(LWIP)/src/include -I$(RTLP)/modchip/bridge \
		'-DMODCHIP_VERSION="test"' -o $@ test_httpd.c ../src/httpd.c $(HTTPD_SRC) $(LWIP_CORE)

# The same with TLS (the SDK's altcp_tls_mbedtls.c, as the firmware builds it): HTTPS
# switched on and off, the listener closed under lwIP's asserts
$(BUILD)/test_httpd_tls: test_httpd.c ../src/httpd.c ../src/tls_cert.c ../src/lwipopts.h lwip_httpd_tls/lwipopts.h mbedtls_test_config.h
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O1 -g -w -DTEST_HTTPD_TLS -Ilwip_httpd_tls -Ilwip_httpd -I. -I../src -I$(LWIP)/src/include \
		-I$(RTLP)/modchip/bridge -I$(LWIP)/src/apps/altcp_tls -I$(MBEDTLS)/include -I$(MBEDTLS)/library \
		'-DMBEDTLS_CONFIG_FILE="mbedtls_test_config.h"' '-DMODCHIP_VERSION="test"' \
		-o $@ test_httpd.c ../src/httpd.c $(HTTPD_SRC) ../src/tls_cert.c $(LWIP_CORE) \
		$(LWIP)/src/apps/altcp_tls/altcp_tls_mbedtls_mem.c \
		$(PICO_SDK_PATH)/src/rp2_common/pico_lwip/altcp_tls_mbedtls.c $(wildcard $(MBEDTLS)/library/*.c)

$(BUILD)/test_tls: test_tls.c ../src/tls_cert.c ../src/tls_cert.h ../src/mbedtls_config.h mbedtls_test_config.h
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O2 -w -I../src -I$(MBEDTLS)/include -I$(MBEDTLS)/library \
		'-DMBEDTLS_CONFIG_FILE="mbedtls_test_config.h"' -I. \
		-o $@ test_tls.c ../src/tls_cert.c $(wildcard $(MBEDTLS)/library/*.c)

# 802.1X against a simulated supplicant and RADIUS server; run_dot1x_radius.sh
# runs test_dot1x_radius against a real FreeRADIUS in Docker (not part of "all")
$(BUILD)/test_dot1x: test_dot1x.c ../src/dot1x.c ../src/md5.c ../src/dot1x.h ../src/md5.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ test_dot1x.c ../src/dot1x.c ../src/md5.c

$(BUILD)/test_dot1x_radius: test_dot1x_radius.c ../src/dot1x.c ../src/md5.c ../src/dot1x.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Wno-unused-result -o $@ test_dot1x_radius.c ../src/dot1x.c ../src/md5.c

dot1x-radius: $(BUILD)/test_dot1x_radius
	sh run_dot1x_radius.sh $<

$(BUILD)/test_link: test_link.c ../src/link.c ../src/link.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_link.c ../src/link.c

$(BUILD)/version.h:
	@mkdir -p $(BUILD)
	printf '#define VERSION_SW "sim"\n#define BUILD_DATE "sim"\n' > $@

$(BUILD)/web_assets.c: ../tools/embed_web.py $(RTLP)/config.txt $(wildcard ../web/*)
	@mkdir -p $(BUILD)
	python3 ../tools/embed_web.py $@ $(RTLP)/config.txt $(RTLP)/html ../web

$(BUILD)/rtl_sfr.o: ../src/rtl_sfr.S
	@mkdir -p $(BUILD)
	$(CC) -c -o $@ $<

$(BUILD)/sim: $(SIM_SRC) $(BUILD)/rtl_sfr.o $(BUILD)/version.h ../src/*.h
	$(CC) $(SIM_FLAGS) -o $@ $(SIM_SRC) $(BUILD)/rtl_sfr.o

# The same for the PoE board's switch: PoE through the bridge's POE_* requests
POE_MACHINE = POE_2G040210GSM
$(BUILD)/sim-poe: $(SIM_SRC) $(BUILD)/rtl_sfr.o $(BUILD)/version.h ../src/*.h
	$(CC) $(subst $(MACHINE),$(POE_MACHINE),$(SIM_FLAGS)) -o $@ \
		$(subst machine/$(MACHINE).c,machine/$(POE_MACHINE).c,$(SIM_SRC)) $(BUILD)/rtl_sfr.o

clean:
	rm -rf $(BUILD)

.PHONY: all clean dot1x-radius
