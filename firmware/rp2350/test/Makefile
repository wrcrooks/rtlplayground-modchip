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
SDK_TESTS = $(if $(PICO_SDK_PATH),$(BUILD)/test_unifi $(BUILD)/test_net6 $(BUILD)/test_usm $(BUILD)/test_update $(BUILD)/test_ssh $(BUILD)/test_mqtt $(BUILD)/test_tls $(BUILD)/test_snmp $(BUILD)/test_httpd $(BUILD)/test_httpd_tls)

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
	node check_extras.js ../web/extras.html
	node check_dashboard.js ../web/dashboard.html $(BUILD)/history.bin $(BUILD)/metrics.json
	python3 -c "import sys; sys.path.insert(0, '../tools'); import embed_web; print(embed_web.SYSTEM_HTTPS + embed_web.DOT1X + embed_web.FEATURES)" > $(BUILD)/system_https.html
	node check_conf_merge.js $(RTLP)/html/app.js $(BUILD)/system_https.html
ifneq ($(PICO_SDK_PATH),)
	python3 run_unifi.py $(BUILD)/test_unifi unifi
	$(BUILD)/test_tls
	$(BUILD)/test_usm
	$(BUILD)/test_net6
	python3 run_snmp.py $(BUILD)/test_snmp
	$(BUILD)/test_httpd
	$(BUILD)/test_httpd_tls
	python3 run_update.py $(BUILD)/test_update $(BUILD)/key.pem $(BUILD)/key2.pem
	python3 run_https.py $(BUILD)/test_httpd_tls
	python3 run_ssh.py $(BUILD)/test_ssh
	python3 run_mqtt.py $(BUILD)/test_mqtt
else
	@echo "SKIP  test_tls, test_snmp, test_httpd: set PICO_SDK_PATH for mbedTLS's and lwIP's sources"
endif

# mbedTLS as the firmware configures it, plus what the tests need
MBED_INC = -I. -I$(MBEDTLS)/include -I$(MBEDTLS)/library '-DMBEDTLS_CONFIG_FILE="mbedtls_test_config.h"'
$(BUILD)/libmbed.a: host_mbed.c mbedtls_test_config.h ../src/mbedtls_config.h
	@mkdir -p $(BUILD)/mbed
	cd $(BUILD)/mbed && $(CC) -std=gnu11 -O2 -w -c -I../.. $(patsubst -I.,,$(MBED_INC)) $(wildcard $(MBEDTLS)/library/*.c) ../../host_mbed.c
	rm -f $@ && ar rcs $@ $(BUILD)/mbed/*.o

$(BUILD)/test_usm: test_usm.c ../src/usm.c ../src/usm.h $(BUILD)/libmbed.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined $(MBED_INC) -o $@ test_usm.c $(BUILD)/libmbed.a

SNMP_SRC = asn1 core mib2 mib2_icmp mib2_interfaces mib2_ip mib2_snmp mib2_system mib2_tcp mib2_udp \
	msg pbuf_stream scalar table traps
LWIP_SRC = $(wildcard $(LWIP)/src/core/*.c) $(wildcard $(LWIP)/src/core/ipv4/*.c) $(wildcard $(LWIP)/src/core/ipv6/*.c) $(LWIP)/src/netif/ethernet.c ../src/tcp_reuse.c \
	$(patsubst %,$(LWIP)/src/apps/snmp/snmp_%.c,$(SNMP_SRC))

$(BUILD)/test_snmp: test_snmp.c $(BUILD)/libmbed.a ../src/usm.c ../src/usm.h ../src/snmp.c ../src/snmp_mib.c ../src/snmp_modchip.h ../src/plat.h ../src/lwipopts.h lwip_host/lwipopts.h
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O1 -g -w -Ilwip_host -I../src -I$(LWIP)/src/include -I$(RTLP)/modchip/bridge \
		$(MBED_INC) '-DMODCHIP_VERSION="sim"' -o $@ test_snmp.c ../src/snmp.c ../src/snmp_mib.c ../src/usm.c ../src/events.c ../src/store.c \
		$(LWIP_SRC) $(BUILD)/libmbed.a

$(BUILD)/test_metrics: test_metrics.c ../src/metrics.c ../src/metrics.h ../src/plat.h ../src/bridge.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(RTLP)/modchip/bridge -o $@ test_metrics.c ../src/metrics.c

$(BUILD)/test_net6: test_net6.c ../src/net.c ../src/events.c ../src/lwipopts.h
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O1 -g -w -fsanitize=address -Ilwip_httpd -I../src -I$(LWIP)/src/include -o $@ test_net6.c ../src/net.c ../src/events.c $(LWIP_CORE)

$(BUILD)/key.pem $(BUILD)/key2.pem:
	@mkdir -p $(BUILD)
	python3 ../tools/sign_update.py keygen $@

$(BUILD)/test_update: test_update.c ../src/update.c ../src/update.h ../src/store.c ../src/events.c $(BUILD)/libmbed.a $(BUILD)/key.pem $(BUILD)/key2.pem
	python3 ../tools/sign_update.py header $(BUILD)/key.pem $(BUILD)/update_key.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -I$(BUILD) $(MBED_INC) -o $@ test_update.c ../src/update.c ../src/store.c ../src/events.c $(BUILD)/libmbed.a

ALTCP_TLS = $(LWIP)/src/apps/altcp_tls/altcp_tls_mbedtls_mem.c $(PICO_SDK_PATH)/src/rp2_common/pico_lwip/altcp_tls_mbedtls.c

$(BUILD)/test_mqtt: test_mqtt.c relay.h ../src/telemetry.c ../src/tls_client.c ../src/rng.c ../src/events.c $(BUILD)/libmbed.a ../src/lwipopts.h
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O1 -g -w -fsanitize=address -Ilwip_httpd_tls -Ilwip_httpd -I../src -I$(LWIP)/src/include -I$(LWIP)/src/apps/altcp_tls $(MBED_INC) \
		-I$(RTLP)/modchip/bridge -o $@ test_mqtt.c ../src/telemetry.c ../src/tls_client.c ../src/rng.c ../src/events.c \
		$(LWIP)/src/apps/mqtt/mqtt.c $(LWIP_CORE) $(ALTCP_TLS) $(BUILD)/libmbed.a

$(BUILD)/test_ssh: test_ssh.c relay.h ../src/ssh.c ../src/ssh.h ../src/ssh_lwip.c ../src/rng.c ../src/events.c ../src/store.c $(BUILD)/libmbed.a ../src/lwipopts.h
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O1 -g -w -fsanitize=address -Ilwip_httpd -I../src -I$(LWIP)/src/include $(MBED_INC) -o $@ test_ssh.c \
		../src/ssh.c ../src/ssh_lwip.c ../src/rng.c ../src/events.c ../src/store.c $(LWIP_CORE) $(BUILD)/libmbed.a

$(BUILD)/test_safety: test_safety.c ../src/events.c ../src/store.c ../src/safety.c ../src/events.h ../src/store.h ../src/safety.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ test_safety.c ../src/events.c ../src/store.c ../src/safety.c

$(BUILD)/test_mcast: test_mcast.c ../src/mcast.c ../src/mcast.h ../src/events.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ test_mcast.c ../src/mcast.c ../src/events.c

$(BUILD)/test_upload: test_upload.c ../src/upload.c ../src/upload.h ../src/link.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_upload.c ../src/upload.c ../src/link.c

LWIP_CORE = $(wildcard $(LWIP)/src/core/*.c) $(wildcard $(LWIP)/src/core/ipv4/*.c) $(wildcard $(LWIP)/src/core/ipv6/*.c) $(LWIP)/src/netif/ethernet.c ../src/tcp_reuse.c

HTTPD_SRC = ../src/upload.c ../src/link.c ../src/events.c ../src/store.c ../src/safety.c ../src/extras.c ../src/update.c

$(BUILD)/test_httpd: test_httpd.c $(HTTPD_SRC) ../src/httpd.c ../src/upload.c ../src/upload.h ../src/lwipopts.h lwip_httpd/lwipopts.h $(BUILD)/libmbed.a
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O1 -g -w -Ilwip_httpd -I../src -I$(LWIP)/src/include -I$(RTLP)/modchip/bridge $(MBED_INC) \
		'-DMODCHIP_VERSION="test"' -o $@ test_httpd.c ../src/httpd.c $(HTTPD_SRC) $(LWIP_CORE) $(BUILD)/libmbed.a

# The same with TLS (the SDK's altcp_tls_mbedtls.c, as the firmware builds it): HTTPS
# switched on and off, the listener closed under lwIP's asserts
$(BUILD)/test_httpd_tls: test_httpd.c relay.h ../src/httpd.c ../src/tls_cert.c ../src/tls_client.c ../src/lwipopts.h lwip_httpd_tls/lwipopts.h $(BUILD)/libmbed.a
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O1 -g -w -DTEST_HTTPD_TLS -Ilwip_httpd_tls -Ilwip_httpd -I. -I../src -I$(LWIP)/src/include \
		-I$(RTLP)/modchip/bridge -I$(LWIP)/src/apps/altcp_tls $(MBED_INC) '-DMODCHIP_VERSION="test"' \
		-o $@ test_httpd.c ../src/httpd.c $(HTTPD_SRC) ../src/tls_cert.c ../src/tls_client.c ../src/rng.c $(LWIP_CORE) \
		$(LWIP)/src/apps/altcp_tls/altcp_tls_mbedtls_mem.c \
		$(PICO_SDK_PATH)/src/rp2_common/pico_lwip/altcp_tls_mbedtls.c $(BUILD)/libmbed.a

$(BUILD)/test_tls: test_tls.c ../src/tls_cert.c ../src/tls_cert.h ../src/tls_client.c ../src/rng.c $(BUILD)/libmbed.a
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O2 -w -Ilwip_httpd_tls -Ilwip_httpd -I../src -I$(LWIP)/src/include $(MBED_INC) -Wl,--gc-sections -ffunction-sections \
		-o $@ test_tls.c ../src/tls_cert.c ../src/tls_client.c ../src/rng.c $(BUILD)/libmbed.a

# 802.1X against a simulated supplicant and RADIUS server; run_dot1x_radius.sh
# runs test_dot1x_radius against a real FreeRADIUS in Docker (not part of "all")
$(BUILD)/test_dot1x: test_dot1x.c ../src/dot1x.c ../src/md5.c ../src/dot1x.h ../src/md5.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ test_dot1x.c ../src/dot1x.c ../src/md5.c

$(BUILD)/test_dot1x_radius: test_dot1x_radius.c ../src/dot1x.c ../src/md5.c ../src/dot1x.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Wno-unused-result -o $@ test_dot1x_radius.c ../src/dot1x.c ../src/md5.c

$(BUILD)/test_radsec: test_dot1x_radius.c relay.h ../src/radsec.c ../src/dot1x.c ../src/tls_client.c $(BUILD)/libmbed.a
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O1 -g -w -fsanitize=address -DRADSEC -Ilwip_httpd_tls -Ilwip_httpd -I../src -I$(LWIP)/src/include -I$(LWIP)/src/apps/altcp_tls $(MBED_INC) \
		-o $@ test_dot1x_radius.c ../src/radsec.c ../src/dot1x.c ../src/md5.c ../src/tls_client.c ../src/tls_cert.c ../src/rng.c ../src/events.c \
		$(LWIP_CORE) $(ALTCP_TLS) $(BUILD)/libmbed.a

dot1x-radius: $(BUILD)/test_dot1x_radius $(BUILD)/test_radsec
	sh run_dot1x_radius.sh $<
	sh run_dot1x_radius.sh - $(BUILD)/test_radsec
	TLS_MAX=1.2 sh run_dot1x_radius.sh - $(BUILD)/test_radsec

$(BUILD)/test_unifi: test_unifi.c ../src/unifi_proto.c ../src/unifi_proto.h ../src/inflate.c ../src/inflate.h $(BUILD)/libmbed.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined $(MBED_INC) -o $@ test_unifi.c ../src/unifi_proto.c ../src/inflate.c $(BUILD)/libmbed.a

# unifi.c against a UniFi Network controller in Docker: make unifi-controller
$(BUILD)/test_unifi_net: test_unifi_net.c relay.h ../src/unifi.c ../src/unifi_proto.c ../src/inflate.c ../src/rng.c ../src/events.c ../src/store.c $(BUILD)/libmbed.a ../src/lwipopts.h
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O1 -g -w -fsanitize=address -DWATCH_S=20 -Ilwip_httpd_tls -Ilwip_httpd -I../src -I$(LWIP)/src/include \
		-I$(LWIP)/src/apps/altcp_tls $(MBED_INC) -o $@ test_unifi_net.c \
		../src/unifi.c ../src/unifi_proto.c ../src/inflate.c ../src/rng.c ../src/events.c ../src/store.c $(LWIP_CORE) $(ALTCP_TLS) $(BUILD)/libmbed.a

unifi-controller: $(BUILD)/test_unifi_net
	python3 run_unifi_controller.py $<

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

.PHONY: all clean dot1x-radius unifi-controller
