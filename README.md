# RTLPlayground mod chip

An RP2350 board inside SWTG024AS-V2.0 switches (MokerLink 2G040210GS, XikeStor SKS3200M-4GPY2XF) that
takes over management from the RTL8372's built-in 8051. The 8051 keeps only what has to run on it,
under a small bridge firmware, and tunnels the rest to the RP2350:

- chip init;
- SerDes and SFP upkeep;
- the CPU-port packet DMA;
- PHY, SerDes and flash access.

**Status:** Not tested as I haven't received the assembled PCBs yet.

The design, the link protocol, the pinouts and the mechanical constraints are in
[ARCHITECTURE.md](ARCHITECTURE.md). To test the firmware before the boards arrive, a Beetle RP2350 on jumper wires
stands in for the mod chip: [docs/beetle-test-rig.md](docs/beetle-test-rig.md).

| Path | What |
|---|---|
| `hardware/` | KiCad 10 schematic and 4-layer board, routed, with clean ERC and DRC. Also the board outline reaching headers T3, T8 and T5, a 1:1 print template, a DXF, photo overlays, STL/STEP for a printed fit check, JLCPCB production files, and the scripts that generate and check all of it. See [hardware/README.md](hardware/README.md) |
| `hardware/poe/` | A second board, 18.9 × 27.5 mm, for the MokerLink POE-2G040210GSM: on its T7 header, with wires to T9. Routed with a clean DRC, JLCPCB files included; its T7/T9 signal pins are still guesses. See [hardware/poe/README.md](hardware/poe/README.md) |
| `firmware/rp2350/` | The RP2350's application firmware in C (Pico SDK 2, lwIP): RTLPlayground's web UI, command line, IP stack, DHCP/DNS/NTP/syslog, STP/LACP/IGMP/LLDP and configuration, plus HTTPS, SSH, IPv6, SNMP, MQTT and Prometheus, 802.1X, multicast snooping, port protection, signed updates and a graphs dashboard, with RTLPlayground's modules compiled unchanged from a submodule. Builds and passes host tests; the base has run on a MokerLink through the Beetle rig, the newer features not on hardware yet. See [firmware/rp2350/README.md](firmware/rp2350/README.md) |
| `firmware/micropython/` | RP2350/RP2040 tools in MicroPython: `read_chip_id.py` reads the switch over T3, `bridge_link.py` is the host side of the bridge protocol, `bridge_host_test.py` talks to the bridge over T5, and `test_bridge_link.py` checks the protocol against the bridge's own frames |

The RP2350's application firmware is in `firmware/rp2350/`; it needs the submodule
(`git submodule update --init`).

## The bridge firmware

The 8051 side lives in RTLPlayground, on the `feat/modchip` branch of the fork:
[`modchip/bridge/`](https://github.com/wrcrooks/RTLPlayground/tree/feat/modchip/modchip/bridge). It shares
RTLPlayground's chip-level modules there. Build it from that repository's root:

```
make -f modchip/bridge/Makefile MACHINE=SWGT024_V2_0_UNMANAGED LAYOUT=compact   # MokerLink, 512 KiB
make -f modchip/bridge/Makefile MACHINE=SWGT024_V2_0_MANAGED                    # XikeStor, 2 MiB
make -f modchip/bridge/Makefile MACHINE=POE_2G040210GSM                         # MokerLink PoE, 2 MiB
```

The PoE switch's controller image goes into that last build with `tools/add_pse_image.py`. The RP2350
firmware for that board is `-DMODCHIP_BOARD=poe`.

## Roadmap

- **UniFi.** The switch as a UniFi switch: adopted by a UniFi Network controller, which sets its
  VLANs, port VLAN IDs, names and speeds, STP, isolation, storm control, egress limits and
  Etherlighting, and sees its clients and LLDP neighbours. In the firmware, tested against a
  controller in Docker but not on a switch yet (`test/run_unifi_hw.py` is ready for that). Next:
  port security, LAG and mirroring, which the controller refuses for the models the switch claims.
  See [firmware/rp2350/README.md](firmware/rp2350/README.md#unifi).
- **Port LED daughter board.** A small board on the mod chip with one WS2812B LED per port, chained,
  in the style of UniFi's Etherlighting. Each LED shows the colour of its port's VLAN, or the link
  speed, and blinks to locate a port, from the command line or from the controller's own locate and
  Etherlighting settings. The firmware side is done (`portled` commands, a PIO driver, colours tested
  on the host; see [firmware/rp2350/README.md](firmware/rp2350/README.md#port-led-board)), with no
  board to drive yet. Open questions:
  - **Placement.** Behind the front panel, over each port, with light pipes where the case has no
    window.
  - **Power.** WS2812B wants 3.5 to 5.3 V and up to 60 mA per LED at full white: a 5 V supply from
    the switch's input supply, or LEDs that run from 3.3 V, with brightness capped in firmware.
  - **Signal.** One spare RP2350 pin, driven by a PIO state machine, with a level shifter if the
    LEDs run at 5 V.
  - **Connector.** The mod chip board has free GPIOs but no header for them yet: the next revision
    needs one for the data line, power and ground.

## Rebuilding the hardware

Needs the KiCad 10 flatpak and a checkout of kicad-agent-kit:

```
KAK=~/Desktop/Workspace/KiCad/kicad-agent-kit hardware/build.sh
```

This goes from the schematic through ERC, layout, Freerouting, DRC and the JLCPCB files to the STL/STEP.

## Tests

```
make -C firmware/rp2350/test PICO_SDK_PATH=...   # framing, a simulated bridge, TLS, SNMP
python3 firmware/micropython/test_bridge_link.py
python3 hardware/check_sch.py         # ERC, and every pin's net against hardware/expected-nets.json
```

## Credits

- [vDorst/rtl-rs](https://github.com/vDorst/rtl-rs), by vDorst: a Rust library and CLI for the RTL837x's
  slave interface. Its I²C transactions are the reference for the firmware's register access over T3
  (`t3 on`, `t3 test`), above all the write format, which RTLPlayground's notes left open. It also
  shows that the PHY, SerDes and SFP engines can be reached through plain registers.
