cmake_minimum_required(VERSION 3.13)

# Pico SDK 2.x: PICO_SDK_PATH in the environment or on the command line
set(PICO_BOARD modchip CACHE STRING "Board")
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
option(MODCHIP_RP2354 "RP2354A build: in-package flash, U5 not fitted" OFF)
if (MODCHIP_RP2354)
    add_compile_definitions(MODCHIP_RP2354)
    set(PICO_FLASH_SIZE_BYTES 2097152)
endif()

include(pico_sdk_import.cmake)
project(modchip C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

# RTLPlayground's board definition: SWGT024_V2_0_UNMANAGED (MokerLink) or
# SWGT024_V2_0_MANAGED (XikeStor); the hardware is the same SWTG024AS board
set(RTLP_MACHINE SWGT024_V2_0_UNMANAGED CACHE STRING "RTLPlayground machine")
set(RTLP ${CMAKE_CURRENT_LIST_DIR}/rtlplayground)
if (NOT EXISTS ${RTLP}/cmd_parser.c)
    message(FATAL_ERROR "rtlplayground/ is empty: git submodule update --init")
endif()

find_package(Git QUIET)
execute_process(COMMAND ${GIT_EXECUTABLE} describe --always --dirty --tags
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE MODCHIP_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if (NOT MODCHIP_VERSION)
    set(MODCHIP_VERSION unknown)
endif()
string(TIMESTAMP BUILD_DATE "%Y-%m-%d %H:%M:%S")
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN})
file(WRITE ${GEN}/version.h.new
    "#ifndef VERSION_H\n#define VERSION_H\n#define VERSION_SW \"modchip-${MODCHIP_VERSION}\"\n#define BUILD_DATE \"${BUILD_DATE}\"\n#endif\n")
configure_file(${GEN}/version.h.new ${GEN}/version.h COPYONLY)

# The web UI, the mod chip's own pages (web/) and the default config
file(GLOB RTLP_HTML ${RTLP}/html/* ${CMAKE_CURRENT_LIST_DIR}/web/*)
add_custom_command(OUTPUT ${GEN}/web_assets.c
    COMMAND python3 ${CMAKE_CURRENT_LIST_DIR}/tools/embed_web.py ${GEN}/web_assets.c ${RTLP}/config.txt
            ${RTLP}/html ${CMAKE_CURRENT_LIST_DIR}/web
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/tools/embed_web.py ${RTLP_HTML} ${RTLP}/config.txt)

# RTLPlayground's portable modules, compiled unchanged with its host-test
# shim, and the files that stand in for its 8051-only parts
add_library(rtlp OBJECT
    ${RTLP}/cmd_parser.c
    ${RTLP}/cmd_editor.c
    ${RTLP}/httpd/page_impl.c
    ${RTLP}/rtl837x_port.c
    ${RTLP}/rtl837x_stp.c
    ${RTLP}/rtl837x_lacp.c
    ${RTLP}/rtl837x_igmp.c
    ${RTLP}/rtl837x_lldp.c
    ${RTLP}/rtl837x_bandwidth.c
    ${RTLP}/rtl837x_storm.c
    ${RTLP}/rtl837x_cabletest.c
    ${RTLP}/rtl837x_phy.c
    ${RTLP}/rtl837x_leds.c
    ${RTLP}/rtl837x_leds_dump.c
    ${RTLP}/rtl837x_pins.c
    ${RTLP}/rtl837x_trap.c
    ${RTLP}/sfp.c
    ${RTLP}/dns.c
    ${RTLP}/dns_cli.c
    ${RTLP}/ntp.c
    ${RTLP}/ntp_cli.c
    ${RTLP}/syslog.c
    ${RTLP}/machine/${RTLP_MACHINE}.c
    src/rtl_env.c
    src/rtl_app.c
    src/rtl_udp.c
    src/rtl_ext.c
    src/rtl_metrics.c
)
target_include_directories(rtlp PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/stub
    ${GEN}
    ${RTLP}
    ${RTLP}/uip
    ${RTLP}/httpd
    ${RTLP}/modchip/bridge)	# bridge_proto.h; its bridge.h is the 8051's, so src/ goes first
target_compile_definitions(rtlp PRIVATE
    RTLP_HOST_TEST
    MACHINE_${RTLP_MACHINE}
    MACHINE_NAME=${RTLP_MACHINE}
    # lwIP has functions of the same names, taking a netif
    dhcp_start=rtlp_dhcp_start
    dhcp_stop=rtlp_dhcp_stop
    # commands of our own, rtl_ext.c
    CMD_PARSER_EXT=rtl_cmd_ext)
target_compile_options(rtlp PRIVATE
    "SHELL:-include ${RTLP}/test/sdcc_shim.h"
    "SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/stub/rtlp_ext.h"
    -fno-strict-aliasing
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable -Wno-sign-compare
    -Wno-pointer-sign -Wno-incompatible-pointer-types -Wno-discarded-qualifiers)

add_executable(modchip
    src/main.c
    src/bridge.c
    src/link.c
    src/net.c
    src/httpd.c
    src/tls.c
    src/tls_cert.c
    src/snmp.c
    src/snmp_mib.c
    src/snmp_udp.c
    src/metrics.c
    src/rtl_sfr.S
    ${GEN}/web_assets.c
    $<TARGET_OBJECTS:rtlp>)
target_include_directories(modchip PRIVATE src ${RTLP}/modchip/bridge)
target_compile_definitions(modchip PRIVATE MODCHIP_VERSION="${MODCHIP_VERSION}" MODCHIP_LINK_BAUD=976562)
target_compile_options(modchip PRIVATE -Wall)
target_link_options(modchip PRIVATE -Wl,--wrap=sfp_read_block -Wl,--wrap=handle_sfp)
# lwIP's SNMP core and MIB-2 groups, without its transport (snmp.c has its own)
set(LWIP_SNMP ${PICO_LWIP_PATH}/src/apps/snmp)
target_sources(modchip PRIVATE
    ${LWIP_SNMP}/snmp_asn1.c
    ${LWIP_SNMP}/snmp_core.c
    ${LWIP_SNMP}/snmp_mib2.c
    ${LWIP_SNMP}/snmp_mib2_icmp.c
    ${LWIP_SNMP}/snmp_mib2_interfaces.c
    ${LWIP_SNMP}/snmp_mib2_ip.c
    ${LWIP_SNMP}/snmp_mib2_snmp.c
    ${LWIP_SNMP}/snmp_mib2_system.c
    ${LWIP_SNMP}/snmp_mib2_tcp.c
    ${LWIP_SNMP}/snmp_mib2_udp.c
    ${LWIP_SNMP}/snmp_msg.c
    ${LWIP_SNMP}/snmp_pbuf_stream.c
    ${LWIP_SNMP}/snmp_scalar.c
    ${LWIP_SNMP}/snmp_table.c
    ${LWIP_SNMP}/snmp_traps.c)

target_link_libraries(modchip
    pico_stdlib
    pico_mbedtls
    pico_lwip_mbedtls
    pico_flash
    pico_rand
    hardware_adc
    hardware_flash
    hardware_uart
    hardware_watchdog
    pico_lwip_core
    pico_lwip_core4
    pico_lwip_netif
    pico_lwip_arch)

pico_enable_stdio_uart(modchip 0)
pico_enable_stdio_usb(modchip 0)
pico_enable_stdio_rtt(modchip 1)
pico_set_program_name(modchip "RTLPlayground mod chip")
pico_set_program_version(modchip "${MODCHIP_VERSION}")
pico_add_extra_outputs(modchip)
