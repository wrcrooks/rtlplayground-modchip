# RP2350 firmware

This firmware takes over everything RTLPlayground's 8051 firmware did above the chip level. The 8051 now
runs only the bridge firmware (RTLPlayground, branch `feat/modchip`, `modchip/bridge/`), and the RP2350
reaches the switch through it over T5.

**Status:** builds and passes its host tests. It hasn't run on hardware yet.

What it runs:

- **Web UI.** RTLPlayground's `html/` UI, unchanged, with the same login, session cookie and JSON
  endpoints. It also serves `/cmd`, `/config` (download and upload) and `/reset`.
- **Command line.** On the console, and through the web UI's command box.
- **IP.** lwIP, with static settings or DHCP. The management VLAN is tagged and filtered as before.
- **Protocols.** DNS, NTP and syslog; STP/RSTP, LACP, IGMP snooping and LLDP.
- **Configuration.** Text commands, replayed after every `HELLO` from the bridge.
- **Switch reset.** The reset button on the switch still works: hold it more than 3 s to restart, more
  than 10 s to restore the default config.

## How it is put together

RTLPlayground's portable modules compile unchanged from the `rtlplayground/` submodule. The
submodule tracks `wrcrooks/RTLPlayground` at `feat/modchip`. The modules are compiled as its host tests
compile them, with `test/sdcc_shim.h` forced in:

- `cmd_parser.c`, `cmd_editor.c`, `httpd/page_impl.c`;
- `rtl837x_port.c`, `_stp.c`, `_lacp.c`, `_igmp.c`, `_lldp.c`, `_bandwidth.c`, `_storm.c`,
  `_cabletest.c`, `_phy.c`, `_leds.c`, `_pins.c`, `_trap.c`;
- `sfp.c`, `dns.c`, `ntp.c`, `syslog.c` and the CLI files;
- the machine definition.

Around them:

| File | What |
|---|---|
| `src/rtl_env.c` | What `rtlplayground.c` and `httpd.c` provided: globals, console helpers, `reg_read_m()`/`reg_write_m()`, `phy_*()`, `sds_*()`, `sfp_read_block()` and the flash API, each access a bridge request |
| `src/rtl_sfr.S` | Storage for the SFR bytes the modules read results from, laid out so `SFR_DATA_U16` overlaps `SFR_DATA_8`/`SFR_DATA_0` as on the 8051 |
| `src/rtl_app.c` | `main()`'s setup after `HELLO`, the tick and second timers, CPU-port frame dispatch (STP, LACP, IGMP, then ARP/IPv4 to lwIP), `tcpip_output()` and the JSON route table |
| `src/rtl_udp.c` | The slice of uIP's UDP API that `syslog.c`, `dns.c` and `ntp.c` use, so they run unchanged over lwIP |
| `src/plat.h` | The seam between the two halves. The shim redefines `__naked`, `__data` and similar words that the SDK and lwIP headers use, so neither half includes the other's headers |
| `src/link.c`, `src/bridge.c` | Bridge protocol v1: COBS/CRC framing, UART RX by interrupt into a 16 KiB ring, one request in flight, unsolicited messages queued and dispatched from the main loop |
| `src/net.c` | lwIP netif over `TX_FRAME`/`RX_FRAME`, the IP settings shared with the command line, lwIP's DHCP behind `dhcp_start()`, UDP sockets for `rtl_udp.c` |
| `src/httpd.c` | The web server on lwIP's raw TCP API |
| `src/main.c` | Power sensing, the 200 Hz tick, the status LED, the config sector, the main loop |
| `tools/embed_web.py` | Gzips `html/` into the image, and embeds `config.txt` as the default config |

**Main loop.** Everything runs from one loop on core 0, as on the 8051. A module's register access
blocks until the bridge replies. Frames and events that arrive meanwhile are queued and handled on the
next pass, never re-entrantly.

**Bridge recovery.** The firmware sends `HELLO` until the bridge answers, then `PING` every second. It
pulses the switch's reset if three pings go unanswered, at most once every 30 s, and only once a bridge
has been seen since power-up, so a switch running other firmware is left alone. After every `HELLO`
the firmware sets up STP, LACP, IGMP and bandwidth control again and replays the config. `HELLO` comes
after either side restarts, or after a beacon shows that the bridge has dropped the host.

**What the 8051 keeps.** The RP2350 doesn't reach these directly:

- SerDes modes, which follow links and SFP modules. `sds_config()` only prints a note.
- The SFP I²C master: `sfp_read_block()` is wrapped at link time to send `SFP_READ`.
- The NIC, which the bridge owns.

The seconds counter register is left to the bridge. Reads of it return the RP2350's uptime.

**Flash.** The modules address a single flash.

- **Saved config** (`CONFIG_START` to `CONFIG_START + CONFIG_LEN`): the last 4 KiB sector of the
  RP2350's flash.
- **Every other address:** the switch's flash, through `FLASH_READ`/`FLASH_ERASE`/`FLASH_WRITE`.
- **Blank config sector:** gets RTLPlayground's `config.txt`, as on first boot of a compact image.

**No back-powering.** No switch-facing pin is set up until GP26 sees the switch's 3.3 V. The pins go
back to isolated if that rail drops, for example with the board on a bench probe and the switch off.

## Not done yet

- **Firmware updates through the web UI.** `/upload` answers 501. The plan:
  - **Bridge:** stage the bridge image with `FLASH_WRITE` and send `REBOOT`; the 8051 installs it.
  - **RP2350 image:** an A/B or bootloader scheme.

  Until then, flash the RP2350 by SWD.
- **Register access over T3 (I²C).** Every access goes over the UART for now. T3 needs its write
  format confirmed on hardware first (ARCHITECTURE.md, bring-up step 2).
- **Speed of whole-table endpoints.** `/vlanlist` reads 3 registers for each of 4096 VLANs: 12 282
  requests. At about 0.3 ms per request that is roughly 4 s, while the main loop serves nothing else.

  The other endpoints cost 0 to 220 requests (`test/sim` prints `COST` lines). Possible fixes:
  - a bulk-read request in the bridge;
  - register reads over T3;
  - skipping VLANs the switch has never had configured.
- **Flash ID commands.** The `flash` command's JEDEC ID, unique ID and security register reads print
  that the bridge doesn't offer them.
- **HTTPS, SNMP.** Not started.

## Build

Pico SDK 2.x (tested with 2.3.1), arm-none-eabi-gcc, CMake and Ninja.

```
git submodule update --init
cmake -S firmware/rp2350 -B firmware/rp2350/build -G Ninja -DPICO_SDK_PATH=/path/to/pico-sdk
ninja -C firmware/rp2350/build          # build/modchip.uf2, .elf, .bin
```

Options:

- `-DRTLP_MACHINE=SWGT024_V2_0_MANAGED` for the XikeStor. The default, `SWGT024_V2_0_UNMANAGED`,
  is the MokerLink.
- `-DMODCHIP_RP2354=ON` for the RP2354A build, which uses the 2 MiB in-package flash.

**Flashing.** There's no USB, so flash over SWD on J5 with a debug probe:

```
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "adapter speed 5000" \
        -c "program firmware/rp2350/build/modchip.elf verify reset exit"
```

**Console.** On RTT over the same probe, for example `openocd ... -c "rtt setup 0x20000000 0x82000
\"SEGGER RTT\"; rtt start; rtt server start 9090 0"`, then `telnet localhost 9090`. Lines from the
8051 show up prefixed `[8051]`.

## Tests

```
make -C firmware/rp2350/test
```

- **`test_link`** checks the framing against the frames the 8051's own `link.c` produces
  (`firmware/micropython/bridge_vectors.txt`).
- **`sim`** runs the RTLPlayground half on the host against a simulated bridge (registers in
  memory, frames recorded). `run_sim.py` then checks:
  - config replay on a blank sector, MAC and hostname from `HELLO`;
  - every JSON endpoint answering valid JSON;
  - `/cmd` replies and errors, DHCP start;
  - the password check;
  - CPU-port frames reaching lwIP with their tags removed, and the management VLAN filter;
  - L2 protocol frames leaving CPU-tagged and IP frames leaving VLAN-tagged;
  - syslog datagrams through the uIP shim.
