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
[ARCHITECTURE.md](ARCHITECTURE.md).

| Path | What |
|---|---|
| `hardware/` | KiCad 10 schematic and 4-layer board, routed, with clean ERC and DRC. Also the board outline reaching headers T3, T8 and T5, a 1:1 print template, a DXF, photo overlays, STL/STEP for a printed fit check, JLCPCB production files, and the scripts that generate and check all of it. See [hardware/README.md](hardware/README.md) |
| `firmware/rp2350/` | The RP2350's application firmware in C (Pico SDK 2, lwIP): RTLPlayground's web UI, command line, IP stack, DHCP/DNS/NTP/syslog, STP/LACP/IGMP/LLDP and configuration, plus HTTPS and an SNMP agent, with RTLPlayground's modules compiled unchanged from a submodule. Builds and passes host tests, not yet run on hardware. See [firmware/rp2350/README.md](firmware/rp2350/README.md) |
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
```

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
