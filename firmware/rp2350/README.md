# RP2350 firmware

This firmware takes over everything RTLPlayground's 8051 firmware did above the chip level. The 8051 now
runs only the bridge firmware (RTLPlayground, branch `feat/modchip`, `modchip/bridge/`), and the RP2350
reaches the switch through it over T5.

**Status:** builds and passes its host tests, and runs on a MokerLink through the Beetle rig. Tried there
(2026-11-24) with two devices: HTTPS with TLS 1.2 and 1.3, SSH, IPv6, SNMPv3 and notifications, MQTT over
TLS, Prometheus, rollback, backup and restore, DHCP snooping and RA guard, IGMP and MLD snooping with
the querier, error-disable, QoS on a congested port, MAB over RADIUS and RadSec with VLANs, and signed
updates with the fallback of an image that doesn't work. "Not done yet" has what hasn't been.

What it runs:

- **Web UI.** RTLPlayground's `html/` UI, unchanged, with the same login, session cookie and JSON
  endpoints. It also serves `/cmd`, `/config` (download and upload), `/reset`, and `/upload`, the
  firmware update for the switch's 8051.
- **Command line.** On the console, and through the web UI's command box.
- **IP.** lwIP, with static settings or DHCP. The management VLAN is tagged and filtered as before.
- **Protocols.** DNS, NTP and syslog; STP/RSTP, LACP, IGMP snooping and LLDP.
- **SSH.** The command line, backup and restore.
- **IPv6.** For management: link-local, SLAAC and a static address.
- **Port protection.** Error-disable with recovery, DHCP snooping, 802.1X with guest, auth-fail and
  RADIUS-assigned VLANs.
- **Multicast.** IGMP and MLD snooping per VLAN, with a querier.
- **Safety.** Timed rollback and restart, configuration backup and restore, signed firmware updates of
  the mod chip with A/B slots.
- **Monitoring.** Events to syslog, SNMP, MQTT and the web UI; Prometheus metrics.
- **UniFi.** Adoption by a UniFi Network controller, which then sets VLANs, port VLAN IDs, port names and
  fixed speeds, and sees the ports' links and counters.
- **HTTPS.** TLS on port 443 next to HTTP on port 80, with a key and self-signed certificate made on
  the device.
- **SNMP.** A read-only SNMPv2c and SNMPv3 agent with notifications: MIB-2, with the switch's ports and their counters in the
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
`CMD_PARSER_EXT` when that is defined, as it is only here. That is how the mod chip's
own commands (`https`, `snmp`, `dot1x`, `mqtt` and so on) reach the parser. The web UI's Save keeps only the commands its `CONF_CMDS` list knows, so
`tools/embed_web.py` adds these to it, and `test/check_conf_merge.js` checks the result against the
submodule's `app.js`.

Around them:

| File | What |
|---|---|
| `src/rtl_env.c` | What `rtlplayground.c` and `httpd.c` provided: globals, console helpers, `reg_read_m()`/`reg_write_m()`, `phy_*()`, `sds_*()`, `sfp_read_block()` and the flash API, each access a bridge request |
| `src/rtl_sfr.S` | Storage for the SFR bytes the modules read results from, laid out so `SFR_DATA_U16` overlaps `SFR_DATA_8`/`SFR_DATA_0` as on the 8051 |
| `src/rtl_app.c` | `main()`'s setup after `HELLO`, the tick and second timers, CPU-port frame dispatch (STP, LACP, IGMP, then ARP/IPv4 to lwIP), `tcpip_output()` and the JSON route table |
| `src/rtl_udp.c` | The slice of uIP's UDP API that `syslog.c`, `dns.c` and `ntp.c` use, so they run unchanged over lwIP |
| `src/rtl_ext.c` | The mod chip's own commands |
| `src/rtl_guard.c` | Error-disable, DHCP snooping and QoS, and events to syslog |
| `src/mcast.c`, `src/rtl_mcast.c` | IGMP and MLD snooping: the protocol, and the switch's side of it |
| `src/dot1x.c`, `src/dot1x_plat.c`, `src/dot1x_udp.c`, `src/radsec.c` | 802.1X: the protocol, the switch's side, RADIUS over UDP and over TLS |
| `src/safety.c` | Timed restart and rollback, backup, restore |
| `src/events.c` | The event log, and who hears of events |
| `src/store.c` | What is kept in flash besides the configuration |
| `src/rtl_metrics.c` | What the dashboard and the SNMP agent read from the switch: ports and counters (one `STAT_READ`), chip temperature, MAC table size, SFP diagnostics, the link change timeline, STP/LACP/IGMP activity |
| `src/plat.h` | The seam between the two halves. The shim redefines `__naked`, `__data` and similar words that the SDK and lwIP headers use, so neither half includes the other's headers |
| `src/link.c`, `src/bridge.c` | Bridge protocol v1: COBS/CRC framing, UART RX by interrupt into a 16 KiB ring, one request in flight, unsolicited messages queued and dispatched from the main loop |
| `src/net.c` | lwIP netif over `TX_FRAME`/`RX_FRAME`, the IP settings shared with the command line, lwIP's DHCP behind `dhcp_start()`, UDP sockets for `rtl_udp.c` |
| `src/httpd.c` | The web server on lwIP's altcp API: plain TCP on port 80, TLS on port 443 |
| `src/tls.c`, `src/tls_cert.c` | The HTTPS key and certificate: made with mbedTLS, kept in a flash sector, loaded into altcp_tls |
| `src/snmp.c`, `src/snmp_mib.c`, `src/snmp_udp.c` | The SNMP agent on lwIP's SNMP core: settings, the MIB, the UDP socket (in place of lwIP's `snmp_raw.c`, so the agent can be switched off) |
| `src/usm.c` | SNMPv3's authentication and encryption |
| `src/telemetry.c`, `src/tls_client.c` | MQTT, and TLS as a client: servers are known by their certificate's fingerprint |
| `src/unifi.c`, `src/unifi_proto.c`, `src/inflate.c` | UniFi: the inform client; packets, the controller's JSON and its configuration as commands; zlib |
| `src/lldp_nbr.c` | LLDP neighbours by port, from the frames trapped while `lldp on`; the `neighbors` command |
| `src/ssh.c`, `src/ssh_lwip.c` | The SSH server: the protocol on mbedTLS, and its connections |
| `src/update.c`, `src/update_flash.c` | Firmware updates of the mod chip: the checks, and the RP2350's A/B slots |
| `src/metrics.c` | The dashboard's sampler, its hour of history, `/metrics.json`, `/history.bin` and Prometheus's `/metrics` |
| `src/extras.c` | `/features.json` |
| `web/dashboard.html`, `web/extras.html` | The Graphs and Features pages |
| `tools/sign_update.py` | Signs firmware updates, and makes the image with the slots |
| `src/main.c` | Power sensing, the 200 Hz tick, the status LED, the config sector, the main loop on its 32 KiB stack |
| `tools/embed_web.py` | Gzips `html/` and `web/` into the image, adds the Graphs and Features links to `index.html`, and embeds `config.txt` as the default config |
| `tools/dashboard_preview.py` | Both pages with simulated data, for working on it without a switch |

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

- **SNMP writes.** The agent is read-only.
- **The Features page in other languages.** It is in English only; RTLPlayground's own pages aren't.
- **UniFi on a switch.** Only tested against a controller in Docker so far. Also: the settings it
  doesn't take yet (see UniFi).
- **Hardware tests.** A whole EAP login (a server's first challenge arrived), the auth-fail VLAN, a
  multicast member on a second port, and SNMP, SSH and so on with the mod chip board itself.

## Register access over T3

Register reads and writes go over the bridge by default. T3, the RTL8372's I²C slave interface at
0x5C, is the other path: I2C1 on GP10/GP11 on the mod chip, I2C0 on GP4/GP5 on the Beetle.

```
t3 test                   compare T3 with the bridge: chip ID, and a write/read-back each way
t3 on [100|400|1000]      plain register access over T3, at that many kHz (default 100)
t3                        on or off, speed, reads, writes and errors
t3 off                    back to the bridge
```

**`t3 test`** is bring-up step 2 (ARCHITECTURE.md).
- **What it does:** reads the chip ID over both paths, then writes a table engine input
  (`TBL_DATA_IN_C`, 0x5CC0) over T3 and reads it back over the bridge, and the other way round. It
  restores the register afterwards. A failed check prints the value read back.
- **What it doesn't do:** move register access anywhere. Run `t3 on` only once it passes.
- **Why a write test:** the write format (register address high byte first, the value low byte first,
  one transfer) comes from [vDorst/rtl-rs](https://github.com/vDorst/rtl-rs). It passes on the
  MokerLink (Beetle rig, 2026-10-24), and 400 kHz runs without errors.
- **Not the seconds counter:** 0x06F4 reads back 0 after any write on the RTL8372, so it can't
  serve as a test register.

**With T3 on:**
- **Plain register access** goes over T3.
- **Batched reads stay on the bridge:** `TBL_READ` and `STAT_READ`, since one bridge request there
  replaces dozens of register accesses.
- **Failures:** a failed transfer falls back to the bridge for that access, and 10 failures in a row
  turn T3 off with a console message.

**Status.** The Bridge Link card on the dashboard shows which path registers use, and `/metrics.json`
has the counts.

## Bridge link diagnostics

For bring-up, when the bridge doesn't answer:

```
bridge                    up or down; requests, timeouts, link errors, resets; UART bytes and frames
bridge log                the 8051's last 2 KiB of output (it prints at boot, before a console is open)
bridge raw                the first 4 KiB received after power-up, as hex, and the frame being decoded
bridge probe              GP0/GP1 as inputs with pull-downs for 2 s: idle level and edges on each
bridge hold on|off        stop sending HELLO and PING, only listen
```

- **`bridge probe`:** the switch's TX idles high, its RX floats low. A running bridge beacons once a
  second, so a connected one shows edges on GP1. No edges with GP1 high: the 8051 isn't sending.
- **`bridge raw`:** COBS leaves text readable, so log lines and the HELLO fields show in the dump.
- **`bridge hold on`:** tells hangs the host triggers from hangs the bridge gets into on its own.

**Saving it.** `t3 on` is a command like any other, so the web UI saves it with the config, and it
comes back on at the next replay.

**Pull-ups.** The mod chip has 4.7 kΩ pull-ups on T3. On a test rig, wire them to the switch's 3.3 V
(T3 pin 4).

## Watchdog

The RP2350's watchdog restarts the mod chip when the firmware stops making progress: a panic (lwIP's
asserts end in one), a hard fault, or a loop that never returns. Without it, any of these needed a
hand on the reset button.

- **Timeout:** 8 s, far above any single wait. Making a certificate takes under 1 s, and
  RTLPlayground's longest delay is 3 s.
- **Fed by:** the main loop, and the waits that have their own time limits: `plat_delay_ms()` and a
  bridge request's reply. So a slow or silent 8051 never sets it off; the bridge's own PING and
  reset handling deals with that.
- **Reported:** `"watchdog_reset": true` in the `modchip` section of `/metrics.json`, "Last Restart:
  Watchdog" on the dashboard's Mod Chip card, and a line on the console at boot.
- **`watchdog test`:** stops the main loop on purpose, to check the watchdog on the hardware. The
  mod chip comes back within 8 s.

## Switch firmware updates

The web UI's firmware upload (`/upload`) updates the **8051** through the bridge.
- **What it takes:** any image in the running bridge's layout. That's either a new bridge build, or
  RTLPlayground itself, to give the switch back its own firmware.
- **The check:** the image's CRC-16 must hold. RTLPlayground's build sets it and its boot code checks
  it, and the web UI checks it in the browser too.

**What happens:**
1. **Staging.** The image streams into the bridge's staging area at `upload_start`. One flash
   operation (a 4 KiB erase or a 256-byte page write) runs per main loop pass, and TCP flow control
   holds the browser back. STP, LACP, the dashboard and the rest keep running through the upload,
   which takes about 2-3 minutes on the MokerLink's W25Q40.
2. **Read-back.** Once the whole body is in, the staged image is read back and its CRC checked again.
3. **Verdict.** The browser gets RTLPlayground's verdict, `OK: checksum verified, rebooting` or
   `NO: <reason>`, only after it has sent the whole body, as with RTLPlayground. Refusals before the
   upload starts, such as no session or a busy server, wait for the body too.
4. **Install.** The bridge reboots, and its boot code checks the CRC once more and installs the
   image, as RTLPlayground does for its own updates. The RP2350 restarts with it.

**Safety:**
- **Failed uploads.** A failed upload (bad CRC, wrong size, a flash error, a client that goes away)
  erases the first staging sector, so nothing installable is left for the next boot.
- **Flash protection.** The bridge accepts erases and writes only above its running image.
- **Install time.** The install at boot takes as long as an RTLPlayground update: about 2.5 minutes
  on the MokerLink. Don't power-cycle the switch until it's back.

## HTTPS

**Web UI.** System > Services has an HTTPS switch (added to RTLPlayground's page by
`tools/embed_web.py`). It shows the running state, the certificate's fingerprint and a link to the
HTTPS address. Turning it off from a page opened over HTTPS asks first. Like the other switches there,
it takes effect at once; Save to flash keeps it.

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

**Protocol.** TLS 1.3 and 1.2 with ECDHE over X25519 or P-256, and AES-128-GCM, ChaCha20-Poly1305 or
AES-256-GCM.

**Memory.** Two TLS connections at a time, about 25 KB each (16 KiB in, 4 KiB out, plus state), or one
when other features leave less. Six more wait for their turn, 20 s at most: browsers open several at
once. MQTT and RADIUS over TLS take as much each. An SSH connection is refused when less than 32 KB is
free. Out of memory ends the connection that asked for it, never the firmware.

**Session resumption.** The server keeps 8 TLS 1.2 sessions for an hour, and gives out tickets for
TLS 1.3. A browser polling the
dashboard resumes them instead of repeating the key exchange.

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
snmp v2c on|off           # SNMPv2c, on at first; off leaves SNMPv3
snmp user <name> auth sha|sha256 <password> [aes <password>]
snmp user <name> off
snmp trap <a.b.c.d> [<user>]   # notifications, v2c or as that SNMPv3 user
snmp trap <a.b.c.d> off
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

**SNMPv3.** Up to four users, with SHA-1 or SHA-256 authentication and AES-128. Passwords have 8 to 64
characters. A user without `aes` is authenticated, not encrypted.

**Notifications.** SNMPv2 traps go to at most two addresses.

## 802.1X port authentication

The switch is the authenticator. It asks a device on a port set to `auto` for its identity
(EAP over EAPOL), and relays the EAP conversation to a RADIUS server (RFC 2865, RFC 3579) until the
server accepts or rejects. The port opens on acceptance. Any EAP method the supplicant and the
server share works, since the switch only relays: PEAP, EAP-TLS, EAP-TTLS, EAP-MD5.

```
dot1x on|off
dot1x radius <a.b.c.d> [<udp port>]     1812 by default
dot1x secret <text>
dot1x radsec <fingerprint>|off          RADIUS over TLS, see below
dot1x radius-vlan on|off                VLANs from the RADIUS server
dot1x reauth <60-86400>|off             periodic re-authentication
dot1x port <n> auto|force-auth|force-unauth
dot1x port <n> mab on|off               MAC authentication (MAB)
dot1x port <n> guest-vlan <1-4094>|off      for devices that don't answer
dot1x port <n> auth-fail-vlan <1-4094>|off  for devices that were refused
dot1x port <n> restart
dot1x                                   status
```

All but `restart` and the status are configuration lines, which Save keeps. The MAC table tab has an
802.1X card with the same settings and each port's state.

- **Until a port is authenticated, it carries only EAPOL.** Port isolation does the fencing. The
  closed port forwards to the CPU port only, and no other port forwards to it. `rtl_env.c` keeps the
  isolation masks the configuration (`isolate`, STP) wrote, and lays the gate over them, so reads
  still return the configured masks. The firmware also drops every other frame from a closed port,
  and flushes the port's learned addresses when it closes. After a restart the masks start from the
  boot defaults and the saved config, and reach the switch only once all of it has been read: a port
  closed before the RP2350 restarted stays closed, and opens to its configured mask.
- **A closed port learns no addresses**, so a device on it can't take over another's entry in the MAC
  table. `rtl_env.c` holds its learning limit (`l2 limit`, which applies once the port opens) at 0.
  With `mab on` the limit is 1 until a frame has shown the device's MAC, as the switch drops what it
  may not learn; `l2 sticky` leaves closed ports alone.
- **EAPOL reaches the RP2350** through the reserved-multicast trap for 01:80:C2:00:00:03 (RMA03,
  register 0x4ed8, by analogy with RTL8367C's register order). The trap is set while 802.1X is on.
  Replies go out CPU-tagged to the one port.
- **MAB.** A device that never answers EAPOL is let in by its MAC instead: after three unanswered
  requests (about 90 s), the source MAC of a frame seen on the port goes to the server as user name
  and password (`aabbccddeeff`, Service-Type Call-Check).
- **Timers**, as 802.1X-2004:
  - 30 s between Request/Identity;
  - 30 s for the supplicant;
  - 5 s per RADIUS try, 3 tries;
  - a 60 s quiet period after a failure.

  The server's Session-Timeout (with Termination-Action RADIUS-Request) overrides `dot1x reauth`.
  Re-authentication keeps the port open until it fails.
- **Requests carry:**
  - User-Name;
  - NAS-IP-Address;
  - NAS-Port (the front port);
  - NAS-Port-Type Ethernet;
  - Called-Station-Id (the switch's MAC);
  - Calling-Station-Id (the device's);
  - Framed-MTU 1400;
  - State;
  - EAP-Message, split at 253 bytes;
  - Message-Authenticator.

  Replies are checked against their Response Authenticator, and against their Message-Authenticator
  whenever EAP is involved.
- **VLANs.** With `radius-vlan on`, an Access-Accept's Tunnel-Type VLAN, Tunnel-Medium-Type 802 and
  Tunnel-Private-Group-ID move the port there, untagged. A guest VLAN opens the port for a device that
  never answered, an auth-fail VLAN for one that was refused. The port goes back when it closes.
  - Tagged frames on a hybrid port aren't restricted by a guest VLAN.
  - Don't save VLANs in the web UI while a port is in such a VLAN: that would save the membership.
- **RadSec.** `dot1x radsec <fingerprint>` talks to the server over TLS (RFC 6614, TCP port 2083), and
  only to one whose certificate has that SHA-256 fingerprint. The switch shows its HTTPS certificate;
  `https cert` prints it, for the server to trust. `https newcert` changes it. The shared secret is
  then `radsec` unless one is set.
- **MAB is a weak check.** A MAC address is easily copied.
- **Port based.** One authenticated device opens the port for everything behind it, as in
  802.1X-2004.
- **Not done:**
  - RADIUS accounting;
  - a second server;
  - more than one device per port.

**Tested.** On a MokerLink: MAB over UDP and RadSec with FreeRADIUS 3.2, an assigned VLAN, the guest VLAN,
a closed port, and EAP up to the server's challenge. On the host:

- **`test/test_dot1x.c`.** The protocol against a simulated supplicant and RADIUS server: success,
  rejection, logoff, MAB, a 900-byte EAP exchange, re-authentication, Session-Timeout, a dead server
  and a wrong secret. It also checks MD5 and HMAC-MD5 against their RFC vectors.
- **`make -C test dot1x-radius`.** The same code against a real FreeRADIUS 3.2 in Docker, over UDP and
  over TLS 1.3 and 1.2: EAP-MD5 accepted and rejected, MAB accepted and rejected, a VLAN assigned.
- **The simulator.** The commands, the trap and isolation registers, the CPU-tagged Request/Identity
  and the Access-Request it sets off.

**On a switch** (the MokerLink on the Beetle rig, one link):
- RMA03 at 0x4ed8 traps EAPOL to the CPU, and only 01:80:C2:00:00:03.
- The gate closes the port.
- Request/Identity goes out, the Response is relayed, and FreeRADIUS 3.2 accepts the Access-Request
  and answers with an EAP-MD5 challenge.

- MAB, end to end: a device without a supplicant on port 4 (a NanoPi) stays closed through three
  unanswered requests. The switch then sends its MAC as Call-Check. FreeRADIUS accepts, and the port
  opens about 90 s after 802.1X was turned on.

An EAP login end to end still needs a supplicant on a second port; EAP so far went as far as
FreeRADIUS's challenge.

## Features page

`extras.html` (Features in the web UI's menu) has the settings and states of what follows. Changes
apply at once, and Save on the switch's page keeps them. Settings given on the command line are kept the
same way.

## Port protection and QoS

```
errdisable                              status
errdisable recovery <30-86400>|off      seconds until a port comes back, 300 at first
errdisable cause bpduguard|l2-limit|link-flap on|off
errdisable port <n> clear
dhcp-snoop on|off                       drop DHCP servers' replies on untrusted ports
dhcp-snoop trust <n> on|off             a port servers may be behind
dhcp-snoop ra on|off                    IPv6 router advertisements too
qos port <n> trust dscp|none            priority from the IPv4 DSCP's class
qos port <n> priority <0-7>|off         priority of what else comes in
```

- **Error-disable.** The port forwards nothing and learns nothing, while its link stays up. BPDU guard
  itself is RTLPlayground's `stp port <n> guard`.
- **DHCP snooping** drops by ACL rule; there is no binding table. Changes take effect within a second.
- **QoS.** The priority is also the queue; the eight queues of a port share it by weights of 1, 2, 4 to
  127, from queue 0 to 7. Links that negotiate pause hold the sender back before a queue fills.

## Multicast snooping

```
mcsnoop on|off                    IGMP and MLD snooping per VLAN
mcsnoop querier on|off
mcsnoop interval <10-3600>        s between queries, 125 at first
mcsnoop unknown flood|router      IPv4 multicast nobody asked for
mcsnoop router <n> on|off         a port to a multicast router
```

It replaces RTLPlayground's `igmp`, which snoops without regard to VLANs: only one of them can be on.

## Configuration safety

```
rollback in <1-1440>       back to the configuration saved now, unless confirmed
rollback confirm
reboot in <1-1440>         in that many minutes
reboot cancel
```

- **Rollback.** Start it before a risky change. Without `rollback confirm` in time the switch restarts
  with the configuration saved when it started. A restart before that rolls back too.
- **Backup and restore.** On the Features page, or `GET /backup` and `POST /restore[?confirm=<min>]`, or
  over SSH. Every line is checked before anything is written. A backup has the passwords in it.

## Events

Link changes, logins, 802.1X, error-disable, configuration and restarts are kept for the Features page
(`/events.json`) and go to syslog, SNMP notifications and MQTT, where those are on.

## MQTT and Prometheus

```
mqtt server <a.b.c.d> [<port>]      1883, or 8883 with TLS
mqtt login <user> <password>|off
mqtt topic <prefix>|off             off: the hostname
mqtt tls <fingerprint>|off          TLS, to the server with that certificate (SHA-256)
mqtt interval <5-3600>
mqtt on|off
prometheus on|off                   GET /metrics without a login
prometheus token <text>|off         but with "Authorization: Bearer <text>"
```

Below the topic prefix are `status` (`online`, or `offline` as the last will), `switch`, `port/<n>` and
`event`, in JSON but for `status`. All but `event` are retained. Nothing is taken from the broker.

## UniFi

```
unifi inform http://<address or name>[:<port>]/inform   the controller, kept in flash (port 8080 usually)
unifi inform auto                                 from DHCP option 43, else http://unifi:8080/inform
unifi on|off
unifi                                             status
unifi forget                                      back to the default key, to be adopted again
```

With `unifi on`, the switch informs the controller every 10 s and shows up there to be adopted. It
claims to be a **USW Flex 2.5G 5** (USWED35) with five ports or fewer, otherwise a **USW Flex 2.5G 8**
(USWED36), the copper ports first and the SFP+ ones as ports 9 and 10. The controller's own device list
has both as microcontroller-based switches. The Features page has the same: on or off, the inform URL
(empty for `auto`), the status, and Forget once adopted.

- **Finding the controller.** With no URL set (`unifi inform auto`), it informs the controller DHCP
  option 43 names (Ubiquiti's sub-option 1, an IPv4 address), else `http://unifi:8080/inform`, as UniFi
  devices do. While UniFi is on, its DHCP requests carry the vendor class `ubnt`, which DHCP servers
  set up for UniFi give option 43 to. Names are looked up with the switch's own DNS client (`dns`);
  the URL it is adopted through is kept, and a controller's `set-adopt` URL replaces it.

- **What the controller sets.** VLANs (tagged, untagged or excluded per port), port VLAN IDs, port
  names and fixed speeds; STP (on or off, RSTP or STP, bridge priority, and per port: in STP or not,
  edge, BPDU guard); port isolation; storm control (broadcast, multicast and unknown unicast, in
  packets/s); and egress rate limits. The controller sends its whole configuration as `system_cfg`
  lines; the switch turns them into its own commands (`vlan`, `pvid`, `port`, `stp`, `isolate`,
  `storm`, `bw out`), each setting only when it changes. Note that UniFi has STP (RSTP) on by
  default, so adoption turns it on unless the site's settings say otherwise.
- **What it sees.** Link, speed and counters per port, memory, uptime; the addresses learned on each
  port (the `l2` table, read at most once a minute), which the controller lists as wired clients on
  their ports; and the LLDP neighbour on each port, heard while `lldp on` (`neighbors` lists them too).
  It can locate the switch: the system LED flashes.
- **Its configuration stays the switch's.** Nothing the controller sets is saved in the switch's own
  configuration. The controller's configuration is kept in a flash sector of its own, and applied
  again on top of the switch's after every restart of the bridge, so a restart doesn't need the
  controller.
- **A change that cuts the switch off.** If the controller isn't reached within 2 minutes of a change,
  the switch restarts with the configuration of before and refuses that configuration version
  afterwards. `unifi` shows which.
- **Firmware upgrades** from the controller are refused and logged; the switch stays adopted.
- **Not taken yet:** port security, LAG, mirroring, IGMP snooping, 802.1X, jumbo frames and the
  management VLAN. The controller doesn't send them, since the switch doesn't claim those features
  (`switch_caps`); for USW Flex 2.5G models it refuses mirroring and LAG anyway. No SSH for the controller, and no L2 discovery: that only lists a device in the
  controller's scan, which then adopts it over SSH; one that informs is listed anyway.

Tested against UniFi Network 10.6.109 in Docker (`make -C test unifi-controller`), not on a switch yet.
`test/run_unifi_hw.py` is the same against a real switch (see Tests).

## SSH

```
ssh on|off
ssh                                 status, the host key's fingerprint
ssh newkey
ssh admin@<switch>                  from a computer: any user name, the web UI's password
ssh admin@<switch> backup > config.txt
ssh admin@<switch> restore 5 < config.txt      rolls back unless confirmed in 5 minutes
```

The server is our own (`src/ssh.c`): curve25519 or P-256 key exchange, an ECDSA host key, AES-GCM or
AES-CTR with HMAC-SHA-256. No keys for login, no file transfer, no forwarding. After wrong passwords,
SSH logins are refused for everybody for 30 s; the web UI isn't affected.

## IPv6

```
ip6 on|off                 link-local and SLAAC addresses
ip6 addr <address>|none    and one of our own (/64)
ip6 gw <address>|auto      the gateway, or as router advertisements say
```

## Firmware updates of the mod chip

Over the network, signed, into the half of the flash that isn't running. It takes some setting up:

```
tools/sign_update.py keygen update.pem                 # keep it secret, and a copy of it
cmake ... -DMODCHIP_UPDATE_KEY=$PWD/update.pem         # firmware that trusts that key
tools/sign_update.py factory build/modchip.bin factory.bin    # partition table and slot A, once per board
openocd ... -c "program factory.bin 0x10000000 verify reset exit"
tools/sign_update.py sign update.pem build/modchip.bin modchip.mcu   # each update
curl -b session=... --data-binary @modchip.mcu http://<switch>/update    # or the Features page
```

On a board with more than 2 MiB of flash, `factory` and `sign` can be given its `--flash-size`.

- **Checked before anything is written:** the signature (ECDSA P-256), that the update is for this board
  and machine, and that it is not older than one already installed. The image itself is checked by its
  hash, from the flash, before the sector that makes it bootable is written.
- **Trial.** The boot ROM starts the new image once. After a minute with the switch answering and the
  network up it is kept; `update keep` does that at once. Should it crash, hang, or not get that far in
  five minutes, the next start is the old firmware's. `update` says which slot runs.
- **Without setup, nothing changes.** A build without a key, or a board without the partition table,
  refuses updates and runs as before. Settings survive either way of flashing.
- **This isn't secure boot.** It keeps foreign updates out that come over the network. Whoever has the
  SWD pins can flash anything; no OTP is burned.

## ACL rules

The `acl` command is RTLPlayground's, from DrDoof's
[logicog/RTLPlayground#520](https://github.com/logicog/RTLPlayground/pull/520), carried on the fork's
`feat/modchip` branch until upstream merges it. `rtlplayground/doc/acl.md` has the whole syntax:
- up to 64 rules on every field the switch can match;
- drop, permit, redirect, copy, mirror, trap, VLAN, priority, PCP and DSCP actions;
- meters, counters and field selectors.

```
acl 1 tcp dport 80 drop 3              keep port 3 off the web pages
acl 2 icmp dip 192.168.10.247 count 0 1
acl meter 1 1000 kbps 16000
acl 3 udp dport 5000 police 1 4
acl 4 icmp dscp 46 1
acl 1 off
acl show                               rules, counters, meters exceeded
```

- **Web UI.** The MAC table tab has an ACL card. It lists the rules as Save would keep them, adds
  and removes rules, and shows `acl show` on request. It doesn't poll, because `acl show` clears
  the meters' exceeded bits.
- **When the 8051 restarts,** the switch's tables reset. `acl_reset()` forgets the rules, and the
  saved configuration sets them up again.
- **On this branch the module is built for the RP2350 only** (`RTLP_ACL`): the 8051 image has no
  room for it, and the mod chip's 8051 only runs the bridge.

**Tested** on the MokerLink (Beetle rig), with a host on port 1 and a NanoPi on port 4:
- drop, permit ahead of a drop, redirect, copy and trap;
- a counter, which counted exactly the pings sent;
- a 64 kbps meter, which cut a ping flood and reported itself exceeded;
- DSCP 46 remarking, seen in the switch's replies;
- the web card: add, list, counters and remove.

`acltbl rule|action <0-95>` still reads and writes the raw table entries. The layout is in
`docs/acl-reverse-engineering.md`.

## Build

Pico SDK 2.x (tested with 2.3.1), arm-none-eabi-gcc, CMake and Ninja.

```
git submodule update --init
cmake -S firmware/rp2350 -B firmware/rp2350/build -G Ninja -DPICO_SDK_PATH=/path/to/pico-sdk
ninja -C firmware/rp2350/build          # build/modchip.uf2, .elf, .bin
```

**Version.** `git describe --always --dirty --tags` of this repository, taken on every build by
`tools/version.cmake`, so a build from an edited tree says `-dirty`. The headers it writes, and so
the build date, change only when that version does.

Options:

- `-DMODCHIP_UPDATE_KEY=<key.pem>`: take firmware updates signed with that key, see above.
- `-DRTLP_MACHINE=SWGT024_V2_0_MANAGED` for the XikeStor. The default, `SWGT024_V2_0_UNMANAGED`,
  is the MokerLink.
- `-DMODCHIP_RP2354=ON` for the RP2354A build, which uses the 2 MiB in-package flash.
- `-DMODCHIP_BOARD=beetle` for a DFRobot Beetle RP2350 wired to the switch as a stand-in for the mod
  chip. It has the same pins, 2 MiB flash and a USB serial console. Wiring and steps are in
  [docs/beetle-test-rig.md](../../docs/beetle-test-rig.md).
- `-DMODCHIP_BOARD=poe` for the mod chip on the MokerLink POE-2G040210GSM
  ([hardware/poe/](../../hardware/poe/README.md)); `RTLP_MACHINE` then defaults to
  `POE_2G040210GSM`. The differences are below.

**The PoE board** (`boards/modchip_poe.h`) uses the same GPIOs as the first board, on other headers.
The UART is on GP0/GP1 to T9, and the slave interface on GP10/GP11 to T7. Its flash is 2 MiB, a
W25Q16JV or an RP2354A's own; the same build runs on both. Otherwise:

- **No T8 header.**
- **No reset driver.** When the bridge stops answering, the firmware reports it on the console but
  can't reset the switch; power-cycle it instead.
- **PoE.** Only the 8051 reaches the RTL8238B PoE controller, and it loads the controller's image at
  boot. `src/rtl_poe.c` implements RTLPlayground's `rtl8238.h` over the bridge's `POE_READ`,
  `POE_PORT` and `POE_LOAD` requests. The `poe` command, the saved `poe port <n> on|off` lines and
  the web UI's PoE tab (`/poe.json`) work as on RTLPlayground. They do nothing unless the bridge's
  HELLO has `HELLO_POE`.
- **The bridge image.** Build it with `MACHINE=POE_2G040210GSM` (RTLPlayground's
  `modchip/bridge/README.md`). Add the PoE image to it with `tools/add_pse_image.py`, or PoE stays
  off.

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
without it they are skipped. `run_snmp.py` also needs net-snmp's command-line tools, `run_ssh.py`
OpenSSH's client, `run_https.py` and `run_update.py` the openssl command, and the `check_*.js` Node.

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
  - `t3 test` against a T3 that doesn't answer, one that takes writes without effect, and one that
    works; `t3 on` moving plain register access over while batched reads stay on the bridge; and a
    failing T3 falling back, then turning itself off;
  - all ports' counters in one `STAT_READ`, the same as counter by counter from an older bridge;
  - link events, IGMP membership from v2 and v3 reports and leaves, STP root and LACP partner
    changes, SFP diagnostics.
- **`test_metrics`** runs `metrics.c` over an hour of simulated traffic. It checks the history ring
  and `/history.bin` against the traffic. `run_metrics.py` checks `/metrics.json`.
- **`check_dashboard.js`** runs the dashboard's own script in Node on those two files. It checks the
  series the charts draw from: rates from counter deltas, packet types, errors, SFP power in dBm, marks,
  the port charts grouped by top speed with each port's state, and no negative rates after a counter
  reset.
- **`test_upload`** streams multipart bodies into a simulated NOR flash, including a real bridge image if
  given its path. It checks:
  - one erase per sector and one write per page, never more than one flash operation per call;
  - the image lands byte for byte, with nothing outside the staging area touched;
  - bad-CRC, too-long, too-short and malformed uploads are refused, as is a failing flash write;
  - after any failure or an abort, nothing installable is left behind.
- **`test_httpd`** runs `httpd.c` and `upload.c` on lwIP with the firmware's options, with a TCP client
  in the same stack over the loopback interface. It checks:
  - login, session cookies on the JSON pages and `/cmd`, and a wrong cookie;
  - firmware uploads as the web UI sends them: a good image staged and answered `OK` before the
    reboot;
  - a bad CRC, a wrong-layout image, and an upload without a login each answered `NO` after the
    whole body, without a reset;
  - a client that gives up halfway leaves the staging area invalidated.
- **`test_tls`** builds mbedTLS on the host with the firmware's configuration, makes a key and
  certificate with `tls_cert.c`, then runs a handshake and a request for each cipher suite and curve.
  The client verifies the certificate and the name. A second connection must resume the session
  without a certificate or key exchange.
- **`test_snmp`** runs the agent on the host: lwIP with the firmware's options, `snmp.c` and
  `snmp_mib.c`, six simulated ports, and an OS socket in place of `snmp_udp.c`. `run_snmp.py` then
  queries it with `snmpget`, `snmpwalk` and `snmpset`. It checks the system group, every `ifTable`
  column, the 64-bit counters, a walk of all of MIB-2, a wrong community, and refused writes.
- **`test_usm`** checks SNMPv3's keys, authentication and encryption; `run_snmp.py` uses them with
  net-snmp.
- **`test_safety`**, **`test_mcast`** and **`test_net6`**: rollback, restart and restore; IGMP and MLD
  snooping; IPv6 addresses and neighbours, and the DHCP options for UniFi (vendor class, option 43).
- **`run_https.py`**, **`run_ssh.py`** and **`run_mqtt.py`** put OpenSSL's client, OpenSSH's client and a
  broker in Python against the firmware's own server and client code on lwIP.
- **`test_update`** takes updates made by `sign_update.py`: good ones, and ones with a changed header,
  signature or image, another key, board or size, cut short, or on failing flash. After none of these
  can the slot boot. It also runs through a trial that succeeds, and ones that don't.
- **`check_extras.js`** runs the Features page's script and checks the command behind every control.
- **`test_unifi`** checks zlib against Python's (every block type, damaged streams), packets both
  ways against Python's AES-CBC and AES-GCM, the JSON reader, and the commands for configurations
  captured from a controller (`test/unifi/`).
- **`make -C test unifi-controller`** runs `unifi.c` on lwIP against UniFi Network 10.6.109 in Docker
  (`ghcr.io/jamesbraid/unifi-network:sim`): adoption, the ports as the controller sees them, VLAN and
  port changes, locate, a refused upgrade, a restart, and a change that cuts the switch off.
- **`run_unifi_hw.py`** does those checks on a real switch:
  `python3 test/run_unifi_hw.py <switch> --key <update.pem>`. It builds the firmware with that key,
  signs it and sends it to the switch, waits for it and keeps it (`--no-flash` skips that). Then it
  starts the controller (or uses `UNIFI_API`/`UNIFI_INFORM`), relays its inform port from this host's
  address on the switch's network (`--relay-port`, 8088 at first), adopts the switch and changes its
  settings. Each change must be taken with every command, and this host must still reach the switch.
  It changes only ports with no link, never this host's. After it, the switch is forgotten, its UniFi
  settings are put back, it is `reset` to its saved configuration, and the command log is cleared.
  `--quick` leaves out the restart and the cut-off check (about 6 minutes). The password comes from
  `MODCHIP_PASSWORD`, else it asks; with root it also sends an LLDP frame. It tests only a switch
  whose mod chip says it is that board (`--board`, `beetle` at first), and never the addresses in
  `MODCHIP_HW_REFUSE`. Dry-run against a fake switch in front of `test_unifi_net` only so far.
- **`run_unifi_fake.py`** (`make -C test unifi-fake`, also in `all`) runs the same against a scripted
  controller, for what a real one rarely sends: the controller found by name or DHCP option 43 when
  no URL is set, and that URL kept on adoption; informs held back until the ports can be read, a new
  key once adopted, `set-adopt` naming the controller by a name, a configuration without a version,
  a damaged saved configuration, and a forget while a change is still being checked. It also checks
  the most heap an inform takes (the harness wraps `malloc`): 28.9 KB, the reply and its text
  unpacked, since both are opened in place; it fails above 32 KB.
- **`make -C test fuzz-unifi`** runs libFuzzer (clang, with ASan and UBSan) over what `unifi.c`
  reads from the network: packets, packet contents sealed with the default key (anyone who can
  answer an inform before adoption can send these), zlib streams, replies' JSON, and `system_cfg`
  into commands. `FUZZ_TIME` is in seconds (300 by default). Crashes land in `test/build/`.
