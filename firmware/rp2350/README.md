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
- **HTTPS.** TLS on port 443 next to HTTP on port 80, with a key and self-signed certificate made on
  the device.
- **SNMP.** A read-only SNMPv2c agent: MIB-2, with the switch's ports and their counters in the
  interfaces group and IF-MIB's `ifXTable`.
- **Graphs.** A dashboard page with animated charts and an hour of history, in the style of
  OPNsense's. It's linked as "Graphs" in the web UI's menu.
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

One change in the submodule serves the mod chip: `cmd_parser.c` hands commands it doesn't know to
`CMD_PARSER_EXT` when that is defined, as it is only here. That is how the `https` and `snmp` commands
reach the parser, so the web UI saves them with the rest of the config.

Around them:

| File | What |
|---|---|
| `src/rtl_env.c` | What `rtlplayground.c` and `httpd.c` provided: globals, console helpers, `reg_read_m()`/`reg_write_m()`, `phy_*()`, `sds_*()`, `sfp_read_block()` and the flash API, each access a bridge request |
| `src/rtl_sfr.S` | Storage for the SFR bytes the modules read results from, laid out so `SFR_DATA_U16` overlaps `SFR_DATA_8`/`SFR_DATA_0` as on the 8051 |
| `src/rtl_app.c` | `main()`'s setup after `HELLO`, the tick and second timers, CPU-port frame dispatch (STP, LACP, IGMP, then ARP/IPv4 to lwIP), `tcpip_output()` and the JSON route table |
| `src/rtl_udp.c` | The slice of uIP's UDP API that `syslog.c`, `dns.c` and `ntp.c` use, so they run unchanged over lwIP |
| `src/rtl_ext.c` | The `https` and `snmp` commands |
| `src/rtl_metrics.c` | What the dashboard and the SNMP agent read from the switch: ports and counters (one `STAT_READ`), chip temperature, MAC table size, SFP diagnostics, the link change timeline, STP/LACP/IGMP activity |
| `src/plat.h` | The seam between the two halves. The shim redefines `__naked`, `__data` and similar words that the SDK and lwIP headers use, so neither half includes the other's headers |
| `src/link.c`, `src/bridge.c` | Bridge protocol v1: COBS/CRC framing, UART RX by interrupt into a 16 KiB ring, one request in flight, unsolicited messages queued and dispatched from the main loop |
| `src/net.c` | lwIP netif over `TX_FRAME`/`RX_FRAME`, the IP settings shared with the command line, lwIP's DHCP behind `dhcp_start()`, UDP sockets for `rtl_udp.c` |
| `src/httpd.c` | The web server on lwIP's altcp API: plain TCP on port 80, TLS on port 443 |
| `src/tls.c`, `src/tls_cert.c` | The HTTPS key and certificate: made with mbedTLS, kept in a flash sector, loaded into altcp_tls |
| `src/snmp.c`, `src/snmp_mib.c`, `src/snmp_udp.c` | The SNMP agent on lwIP's SNMP core: settings, the MIB, the UDP socket (in place of lwIP's `snmp_raw.c`, so the agent can be switched off) |
| `src/metrics.c` | The dashboard's sampler, its hour of history, `/metrics.json` and `/history.bin` |
| `web/dashboard.html` | The dashboard page |
| `src/main.c` | Power sensing, the 200 Hz tick, the status LED, the config sector, the main loop on its 32 KiB stack |
| `tools/embed_web.py` | Gzips `html/` and `web/` into the image, adds the Graphs link to `index.html`, and embeds `config.txt` as the default config |
| `tools/dashboard_preview.py` | The dashboard with simulated data, for working on it without a switch |

**Main loop.** Everything runs from one loop on core 0, as on the 8051. A module's register access
blocks until the bridge replies. Frames and events that arrive meanwhile are queued and handled on the
next pass, never re-entrantly. The loop runs on a 32 KiB stack in RAM, with the stack limit register
set at its bottom: mbedTLS needs more than the 4 KiB scratch bank the SDK gives core 0.

**Table reads.** Reading a VLAN or L2 table entry takes three steps: write the command, poll until
it's done, read the result. Over the bridge, each step would be a round trip. Instead, a read command
goes out as one `TBL_READ` request, and the poll and result reads are answered from its reply until
the next register write. VLAN entries are fetched 64 at a time, so `/vlanlist`, a walk over all 4094
VLANs, takes 64 requests instead of 12 282. A bridge from before `TBL_READ` answers "unknown", and the
firmware falls back to plain register access.

**Bridge recovery.** The firmware sends `HELLO` until the bridge answers, then `PING` every second. It
pulses the switch's reset if three pings go unanswered, at most once every 30 s, and only once a bridge
has been seen since power-up, so a switch running other firmware is left alone. After every `HELLO`
the firmware sets up STP, LACP, IGMP and bandwidth control again and replays the config. `HELLO` comes
after either side restarts, or after a beacon shows that the bridge has dropped the host.

**What the 8051 keeps.** The RP2350 doesn't reach these directly:

- **SerDes.** The 8051 sets the modes, following links and SFP modules. `sds_config()` sends
  `SDS_CONFIG`. A speed forced with `sfp <slot> <speed>` is applied that way, at once and again
  whenever a module is inserted; `handle_sfp()` is wrapped at link time for this.
- **The SFP I²C master.** `sfp_read_block()` is wrapped at link time to send `SFP_READ`.
- **The flash's ID registers.** The `flash j|u|s` commands send `FLASH_ID`; the 8051 runs
  RTLPlayground's own dump functions and returns what they print.
- **The NIC**, which the bridge owns.

The seconds counter register is left to the bridge. Reads of it return the RP2350's uptime.

**Flash.** The modules address a single flash.

- **Saved config** (`CONFIG_START` to `CONFIG_START + CONFIG_LEN`): the last 4 KiB sector of the
  RP2350's flash.
- **Every other address:** the switch's flash, through `FLASH_READ`/`FLASH_ERASE`/`FLASH_WRITE`.
- **Blank config sector:** gets RTLPlayground's `config.txt`, as on first boot of a compact image.
- **HTTPS key and certificate:** the sector below the config.

**No back-powering.** No switch-facing pin is set up until GP26 sees the switch's 3.3 V. The pins go
back to isolated if that rail drops, for example with the board on a bench probe and the switch off.

## Not done yet

- **Firmware updates through the web UI.** `/upload` answers 501. The plan:
  - **Bridge:** stage the bridge image with `FLASH_WRITE` and send `REBOOT`; the 8051 installs it.
  - **RP2350 image:** an A/B or bootloader scheme.

  Until then, flash the RP2350 by SWD.
- **Register access over T3 (I²C).** Every access goes over the UART for now. T3 needs its write
  format confirmed on hardware first (ARCHITECTURE.md, bring-up step 2).
- **TLS 1.3.** HTTPS speaks TLS 1.2. Every current browser accepts that.
- **SNMP writes, v3 and traps.** The agent is read-only SNMPv2c.

## HTTPS

```
https on          # TLS on port 443; HTTP on port 80 stays
https             # on or off, and the certificate's SHA-256 fingerprint
https newcert     # a new key and certificate
https off
```

**Certificate.** The first `https on` makes an ECDSA P-256 key and a self-signed certificate, named
after the hostname at that moment, and stores them in flash. That should take about a second (not
measured on hardware yet). Browsers
warn about a self-signed certificate, so compare the fingerprint `https` prints with the one the
browser shows, the first time.

**Protocol.** TLS 1.2 with ECDHE over X25519 or P-256, and AES-128-GCM, ChaCha20-Poly1305 or
AES-256-GCM.

**Memory.** Two TLS connections at a time, about 25 KB each (16 KiB in, 4 KiB out, plus state).

**Session resumption.** The server keeps 8 TLS sessions for an hour. A browser polling the
dashboard resumes them instead of repeating the key exchange. A third waits for one to close.

## Graphs

`dashboard.html` (Graphs in the web UI's menu). The charts scroll smoothly; hover for values, and
click a legend entry to hide a series. The window is 5 minutes, 15 minutes or an hour (`#300`, `#900`,
`#3600` in the address).

| Chart | What it shows | History |
|---|---|---|
| Traffic | All ports stacked, bits/s in above the line, out below | yes |
| Ports | One chart per top speed (2.5 Gb/s copper, 10 Gb/s SFP+), the ports' in and out overlaid; each port's current state (link speed, down or off) in the legend | yes |
| Packets | Packets/s over all ports: unicast, multicast and broadcast, in and out | yes |
| Errors and discards | Per second, all ports | yes |
| Temperatures | Switch chip, mod chip (the RP2350's sensor), SFP modules | chip and mod chip |
| MAC table | Learned addresses | yes |
| SFP modules | Temperature, supply voltage, laser bias, TX and RX power in dBm, LOS; the powers charted | live only |
| Mod chip | Main loop load, heap use, temperature, uptime | load and heap |
| Bridge link | Link load (the busier direction of the UART), CPU-port frames/s in and out, requests/s, timeouts, link errors, frames the 8051 dropped | load and frames/s |
| Protocols | STP state, root port and topology changes (marked on the chart), LACP partner changes, IGMP groups, and joins and leaves per minute | IGMP groups |

**Data.** The page loads `/history.bin` once, then polls `/metrics.json` every 2 s. Rates come
from counter deltas between polls, so a missed poll costs no accuracy.

**Sampling.** The firmware samples every 10 s whether anyone is watching, and keeps an hour of
samples in RAM; they're lost at a reboot.
- **RAM:** about 65 KB for six ports.
- **`/history.bin`:** the same 65 KB, so about 0.7 s over the bridge link when the page opens.
- **Layout:** in `src/metrics.c`.

**Cost per poll.** A poll costs about 17 bridge requests:
- **Counters:** one `STAT_READ` for all ports.
- **Link state:** 3.
- **Port enable:** one PHY read per copper port.
- **Chip temperature and MAC table size:** the rest.

SFP diagnostics are read every 30 s. Polls closer together than a second share one reading.

**Port top speeds.** RTLPlayground's machine definitions don't say, so the firmware assumes SFP+
for the cages and 2.5G for the copper ports, as on the SWTG024AS. A board with 10G copper PHYs would
show those ports in the 2.5 Gb/s chart.

**Sources of the less obvious numbers:**
- **IGMP groups:** counted from the reports and leaves the CPU port sees while snooping is on;
  membership ages out after 260 s.
- **LACP partner changes:** a port's partner system, key or sync bit changing.
- **Main loop load:** the share of time spent on loop passes that had something to do.

**Previewing.** `python3 firmware/rp2350/tools/dashboard_preview.py` serves the page with
simulated data at http://localhost:8080/dashboard.html.

## SNMP

```
snmp community <name>     # default "public"
snmp location <text>
snmp contact <text>
snmp on                   # UDP port 161
snmp                      # the settings
snmp off
```

**What it answers.** SNMPv2c reads: `get`, `getnext`, `getbulk`; the agent accepts no writes. MIB-2
comes from lwIP's groups: system (`sysName` is the hostname), at, ip, icmp, tcp, udp and snmp. The
interfaces are the switch's ports, numbered as on the front panel:

| Table | Columns |
|---|---|
| `ifTable` | descriptions, speeds, MAC, admin and oper status, and the 32-bit counters: octets, unicast and non-unicast packets, discards and errors |
| `ifXTable` | `ifName`, the 64-bit (HC) octet and packet counters, `ifHighSpeed`, and `ifAlias` (the port's name from the web UI) |

**Counters.** They come from the ASIC's MIB counters, read again at most once a second.

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
make -C firmware/rp2350/test PICO_SDK_PATH=/path/to/pico-sdk
```

`test_tls` and `test_snmp` build mbedTLS and lwIP from the SDK, so they need `PICO_SDK_PATH`;
without it they are skipped. `run_snmp.py` also needs net-snmp's command-line tools, and
`check_dashboard.js` needs Node.

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
  - syslog datagrams through the uIP shim;
  - `/vlanlist` through `TBL_READ`, in 64 requests: the same list as entry by entry, and no stale
    entries after a write;
  - `flash j` through `FLASH_ID`;
  - a forced SFP speed reaching `SDS_CONFIG`;
  - the `https` and `snmp` commands, and the history that saves them;
  - the ports as the SNMP agent reads them;
  - all ports' counters in one `STAT_READ`, the same as counter by counter from an older bridge;
  - link events, IGMP membership from v2 and v3 reports and leaves, STP root and LACP partner
    changes, SFP diagnostics.
- **`test_metrics`** runs `metrics.c` over an hour of simulated traffic. It checks the history ring
  and `/history.bin` against the traffic. `run_metrics.py` checks `/metrics.json`.
- **`check_dashboard.js`** runs the dashboard's own script in Node on those two files. It checks the
  series the charts draw from: rates from counter deltas, packet types, errors, SFP power in dBm, marks,
  the port charts grouped by top speed with each port's state, and no negative rates after a counter
  reset.
- **`test_tls`** builds mbedTLS on the host with the firmware's configuration, makes a key and
  certificate with `tls_cert.c`, then runs a handshake and a request for each cipher suite and curve.
  The client verifies the certificate and the name. A second connection must resume the session
  without a certificate or key exchange.
- **`test_snmp`** runs the agent on the host: lwIP with the firmware's options, `snmp.c` and
  `snmp_mib.c`, six simulated ports, and an OS socket in place of `snmp_udp.c`. `run_snmp.py` then
  queries it with `snmpget`, `snmpwalk` and `snmpset`. It checks the system group, every `ifTable`
  column, the 64-bit counters, a walk of all of MIB-2, a wrong community, and refused writes.
