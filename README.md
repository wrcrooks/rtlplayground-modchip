# Mod chip

An RP2040 board inside SWTG024AS-V2.0 switches that takes over management from the RTL8372's 8051.
The design, the link protocol, the pinouts and the mechanical constraints are in
[ARCHITECTURE.md](ARCHITECTURE.md). Nothing here has run on hardware yet.

| Path | What |
|---|---|
| `pico/read_chip_id.py` | MicroPython: reads the chip ID and link state over header T3 (I²C 0x5C). Read-only |
| `pico/bridge_link.py` | MicroPython/CPython: the host side of the bridge protocol |
| `pico/bridge_host_test.py` | MicroPython: talks to the bridge firmware over header T5 |
| `bridge/` | The minimal 8051 firmware: init, SerDes/SFP upkeep, frame tunnel, SFR-only operations |
| `hardware/` | Board outline reaching T3, T8 and T5: generator and checker, DXF for KiCad, 1:1 print template |

Build the bridge firmware from the repository root:

```
make -f modchip/bridge/Makefile MACHINE=SWGT024_V2_0_UNMANAGED LAYOUT=compact
```

Tests: `make -C test` runs `test_bridge_link` (the 8051 framing code, compiled unchanged) and
`test_bridge_link.py` (the Python host side against it). `modchip/bridge/check_hal.py` checks that the
helpers copied from `rtlplayground.c` haven't drifted.
