# Mod chip: an RP2040 management processor for SWTG024AS-V2.0 switches

Status: design draft. The firmware builds and its link framing is tested on the host, but none of this
has run on hardware yet. Measurements marked *photo* come from the board photo in
`doc/devices/photos/SWTG024AS-v2.0-unmanaged/` and must be checked with calipers before a PCB is
ordered.

## Goal

Move the switch's management plane off the RTL8372's built-in 8051 onto an RP2040 on a small board
inside the chassis. The 8051 keeps only what has to run on it. Target boards: the SWTG024AS-V2.0
family, and in particular the two on hand:

| Switch | PCB | Flash | Bridge build |
|---|---|---|---|
| MokerLink 2G040210GS | SWTG024AS-V2.0, unmanaged layout | W25Q40, 512 KiB | `MACHINE=SWGT024_V2_0_UNMANAGED LAYOUT=compact` |
| XikeStor SKS3200M-4GPY2XF | SWTG024AS-V2.0, managed layout | FM25Q16, 2 MiB | `MACHINE=SWGT024_V2_0_MANAGED` |

`doc/devices/SWTG024AS.md` lists the XikeStor as a v1.0 PCB, but the board on hand is marked v2.0.
Check the header positions on both boards before committing to one outline.

## Why the 8051 can't simply be bypassed

The 8051 is part of the RTL8372 die. Three things are reachable only through its special function
registers, so it can't be removed or replaced:

- **the CPU port's packet buffers**, which it reads and writes by DMA (`SFR_NIC_*`). Every frame to or
  from the management plane passes through it;
- **PHY (MDIO) and SerDes access**, which are SFR commands (`SFR_EXEC_READ_SMI`,
  `SFR_EXEC_READ_SDS`);
- **the SPI flash** holding its own code.

Everything else is an ordinary 32-bit switch register. An external host reaches those through the
slave interface on header T3 (I²C, address 0x5C).

The design therefore splits the work. The 8051 runs a small **bridge firmware**: it initialises the
chip, keeps the SerDes and SFPs working, and tunnels frames and SFR-only operations to the RP2040 over
its UART. The RP2040 does everything else.

## System overview

```
                         SWTG024AS-V2.0 switch PCB
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  RTL8372 SoC                                                                 │
 │  ┌──────────────────────────────┐        ┌─────────────────────────────────┐ │
 │  │ switch core: forwarding,     │◄──────►│ 8051 + bridge firmware          │ │
 │  │ VLAN, L2, ACL, counters      │  regs  │  init, SerDes/SFP upkeep,       │ │
 │  │                              │        │  CPU-port DMA, PHY/SDS/flash    │ │
 │  │  slave interface ◄─┐         │        │  UART0 ──┐                      │ │
 │  └────────────────────┼─────────┘        └──────────┼──────────────────────┘ │
 │          T3 (I²C 0x5C)│      T8 (GPIO46-49)│        │T5 (UART0, ~1 Mbaud)    │
 └───────────────────────┼────────────────────┼────────┼────────────────────────┘
                         │                    │        │  3-wire harness
 ┌───────────────────────┴────────────────────┴────────┴────────────────────────┐
 │ mod chip: RP2040, 2 MiB QSPI flash, power path, reset driver, USB-C (dev)    │
 │  core 1: link driver (UART DMA, COBS/CRC), register access (I²C / RPC)       │
 │  core 0: IP stack, web UI, STP/LACP/IGMP/LLDP, configuration                 │
 └──────────────────────────────────────────────────────────────────────────────┘
```

### What runs where

| Function | 8051 (bridge) | RP2040 |
|---|---|---|
| Clock, flash controller, chip init, SerDes and PHY bring-up | ✓ | |
| Forwarding defaults (VLAN 1, all ports reachable, learning) at boot | ✓ | |
| SerDes mode on link change (RTL8221) and SFP insertion | ✓ | |
| SFP detect/LOS polling | ✓, reports changes | |
| CPU-port frame DMA | ✓, tunnels frames | |
| PHY, SerDes, SFP EEPROM and flash access | ✓, on request | requests them |
| Staged firmware install at boot (existing path) | ✓ | stages the image |
| VLANs, port settings, L2/ACL tables, mirroring, storm control, bandwidth | | ✓ (registers) |
| Traps: which frames reach the CPU port | | ✓ (registers) |
| IP stack (ARP, ICMP, DHCP, TCP), web UI, SNMP, syslog, NTP, DNS | | ✓ |
| STP/RSTP, LACP, IGMP snooping and relay, LLDP | | ✓ |
| Configuration storage | | ✓ (its own flash) |
| Console | | ✓ (USB CDC) |

The 8051 sets up the same forwarding defaults RTLPlayground starts from. With no RP2040 present, or
with one that has crashed, the switch therefore still forwards as an unmanaged switch. Registers the
RP2040 has written stay in effect until the next reset.

RTLPlayground's protocol code (`rtl837x_port.c`, `rtl837x_stp.c`, `rtl837x_lacp.c`,
`rtl837x_igmp.c`, …) talks to the chip only through `reg_read_m()`/`reg_write_m()`, `sfr_data` and
`phy_*()`. A shim that maps those onto I²C and bridge requests would let much of it compile for the
RP2040 largely unchanged. `test/sdcc_shim.h` already does the equivalent for the host tests.

## Interfaces

### T3: slave interface (register access)

| T3 pin | Signal | Mod chip |
|---|---|---|
| 1 (square) | SCL / SCK / MDC | RP2040 GP5 (I2C0 SCL), 100 Ω series |
| 2 | GND | GND |
| 3 | SDA / SDI / MDIO | RP2040 GP4 (I2C0 SDA), 100 Ω series |
| 4 | VCC (3.3 V, verify) | power in, through an ideal diode |
| 5 | SPI DO (through 33 Ω) | GP6, for SPI-mode experiments only |
| 6 | SPI CS | GP7, likewise |

- **Mode and address.** The `IF_SEL` straps put this board in I²C mode at 0x5C. The RP2040 has no pin
  that is both an I²C SCL and an SPI SCK, so SPI mode would have to run on PIO. That's not needed for
  this board.
- **Register format.** The address is 16 bits, high byte first; data is 32 bits, low byte first (see
  `doc/devices/SWTG024AS.md`). `pico/read_chip_id.py` checks the read path.
- **Write format.** Unconfirmed: the doc shows the device address repeated mid-write. Check it on the
  register the bridge leaves unused (`RTL837X_REG_SEC_COUNTER`, 0x06f4) before relying on writes.
- **Pull-ups.** The mod chip carries 4.7 kΩ pull-ups to the switch's 3.3 V. That's fine alongside any
  the switch board may have.
- **Empty footprints U4 and U10.** These sit on the same lines: U4 is an I²C EEPROM and U10 an SPI
  part, both for Realtek's configuration-EEPROM autoload, which this board doesn't use. Leave both
  empty.
- **Speed.** A register read is about 8 bytes on the bus: about 0.75 ms at 100 kHz and 0.2 ms at
  400 kHz. That's close to a `REG_READ` over the UART link, about 18 bytes or 0.2 ms at 976 562 baud.
  Fast enough for configuration and counters, not for anything per packet.

### T5: UART (frames and SFR-only operations)

| T5 pin | Signal | Mod chip |
|---|---|---|
| 1 (square) | U0TXD, switch output | RP2040 GP1 (UART0 RX) |
| 2 | GND | GND |
| 3 | U0RXD, switch input | RP2040 GP0 (UART0 TX), 100 Ω series |
| 4 | 3.3 V | not connected |

- **Wiring.** T5 is on the opposite edge from T3, about 90 mm away (*photo*), so it connects through a
  3-wire harness. On the mod chip end that's a JST-SH 1.0 mm socket or solder pads; on the switch end,
  a 1×4 2.54 mm housing on a header soldered into T5.
- **Baud rate.** The bridge runs UART0 at 976 562 baud (`LINK_TH1 0xfe`), which the 125 MHz 8051
  generates exactly. So does the RP2040, from a 125 MHz `clk_peri` with a divisor of 8.
- **Faster option.** 1 953 125 baud (`0xff`, RP2040 divisor 4) is the next step, if the 8051 keeps up
  with its per-byte CRC and COBS work. Measure it on hardware.
- **Throughput.** About 97 KB/s each way at 976 562 baud. A 1500-byte frame takes 15 ms.

### T8: SoC GPIOs (sideband, optional)

| T8 pin | Signal | Mod chip |
|---|---|---|
| 1 (square) | GPIO46 (I2C_SCL0) | GP8, 100 Ω |
| 2 | GND | GND |
| 3 | GPIO48 (I2C_SCL1) | GP10, 100 Ω |
| 4 | 3.3 V | power in, through the same ideal diode |
| 5 | GPIO47 (I2C_SDA0) | GP9, 100 Ω |
| 6 | GPIO49 (I2C_SDA1) | GP11, 100 Ω |

These pins are the SoC's I²C master buses 0 and 1, unused on this board. Protocol v1 doesn't use them.
They're wired because they cost nothing: T8 sits on the same 2.54 mm grid as T3 with one empty
position between them, so one connector takes both.

Planned uses:

- an 8051 → RP2040 "attention" line, to cut latency without polling;
- an RP2040 → 8051 strap read at boot, for example to start RTLPlayground instead of the bridge for
  recovery;
- possibly a second data channel, with the SoC's hardware I²C master talking to the RP2040 as an I²C
  slave.

Their default states and any pull-ups on the switch side are unverified.

### Reset (optional fly-wire)

The SoC reset line is active low, at the cathode of T-D3 (33 kΩ pull-up R78, RC delay C187). It sits
about 10 mm in front of T8 (*photo*). The mod chip pulls it low through a 2N7002 from GP12, which lets
the RP2040 restart a hung 8051 without a power cycle. The pad on the mod chip takes a short wire to
T-D3. Without that wire, everything else still works.

## Bridge firmware (8051)

The source is in `modchip/bridge/`. Build it from the repository root:

```
make -f modchip/bridge/Makefile MACHINE=SWGT024_V2_0_UNMANAGED LAYOUT=compact   # MokerLink, 512 KiB
make -f modchip/bridge/Makefile MACHINE=SWGT024_V2_0_MANAGED                    # XikeStor, 2 MiB
```

The output is `output/bridge-<MACHINE>[-compact]/bridge-<git>-<MACHINE>[-compact].bin`, plus
`…-flash512k.bin` for a programmer writing a whole 512 KiB chip.

| File | Contents |
|---|---|
| `bridge.c` | `main()`, the ISRs, the request handlers, the frame tunnel, link and SFP events |
| `link.c`, `link.h` | COBS framing and CRC, compiled unchanged by `test/test_bridge_link.c` |
| `bridge_proto.h` | message types and status codes |
| `hal.c` | RTLPlayground's register, PHY, SerDes and print helpers, copied unchanged. `check_hal.py` reports any drift from `rtlplayground.c` |
| `Makefile` | links the bridge with the shared chip-level modules: `boot.c`, `sfp.c`, `rtl837x_init.c`, `rtl837x_phy.c`, `rtl837x_pins.c`, `rtl837x_leds.c`, `rtl837x_flash.c`, `rtl837x_nic.c`, `rtl837x_port.c`, `rtl837x_trap.c`, the machine definitions |

The image is about 34 KB of code, against about 142 KB for RTLPlayground, and uses 7.4 KB of the
48 KB XRAM. It leaves out uIP, the web UI, the command line, the config replay, DHCP, DNS, NTP,
syslog, STP, LACP, IGMP and LLDP. Its layout matches an RTLPlayground image, so the boot ROM, a
programmer and the staged update path treat it the same way.

The main loop:

1. **Every tick (200 Hz).** Poll the SFP pins every 50 ms; `handle_sfp()` reconfigures the SerDes for an
   inserted module. Once a second, re-check links, time the host out, and send a beacon while no host
   is present.
2. **On a link interrupt.** Adjust the SerDes as RTLPlayground does, and report the change.
3. **On a NIC interrupt.** DMA up to 4 frames out of the RX ring and send each to the host, or drop
   it if no host is present.
4. **Always.** Decode queued UART bytes and act on one complete request. Flush buffered log text.

The ISRs only latch events and queue UART bytes, as in RTLPlayground. All register access stays in the
main loop.

### Link protocol (v1)

**Framing.** Each message is `type, seq, payload…, CRC-16` encoded with COBS and ended by `0x00`.

- The CRC is CRC-16/MODBUS: reflected polynomial 0xA001, initial value 0xFFFF, appended low byte
  first. It's the same table `crc16.asm` already uses for image checks.
- Multi-byte fields are little endian.
- The largest message is a TX frame: 1 + 1 + 1 + 1556 + 2 bytes.

**Flow control.** The host sends one request, waits for the reply (same `seq`, type `request | 0x80`,
status byte first), then sends the next. That keeps the 8051's 2 KiB receive ring from overflowing
without any credit scheme. The RP2040 receives into a DMA ring and needs no flow control. If the ISR
does lose a byte, the message is dropped and counted in `link_errors`.

| Request | Payload | Reply payload | Notes |
|---|---|---|---|
| `0x01 HELLO` | | hello | Marks the host present; followed by `LINK` and `SFP` |
| `0x02 PING` | | `uptime_s:4 rx_dropped:2 link_errors:2` | Send at least every 3 s (`HOST_TIMEOUT_S`) |
| `0x10 TX_FRAME` | `flags:1 frame` | | `flags` is the NIC descriptor's checksum byte, normally 0x07. The frame runs from the destination MAC, without FCS; insert an 0x8899 CPU tag to direct it (`doc/CpuPort.md`) |
| `0x20 REG_READ` | `reg:2` | `value:4` | The same register the slave interface reaches |
| `0x21 REG_WRITE` | `reg:2 value:4` | | |
| `0x22 PHY_READ` | `phy:1 dev:1 reg:2` | `value:2` | Clause 45; `phy` < 16 |
| `0x23 PHY_WRITE` | `phy:1 dev:1 reg:2 value:2` | | |
| `0x24 SDS_READ` | `sds:1 page:1 reg:1` | `value:2` | `sds` ≤ 1, `page` < 128 |
| `0x25 SDS_WRITE` | `sds:1 page:1 reg:1 value:2` | | |
| `0x26 SFP_READ` | `slot:1 offset:1 len:1` | `data` | `len` 1-16; bit 7 of `offset` selects A2h (diagnostics) |
| `0x30 FLASH_READ` | `addr:4 len:2` | `data` | `len` 1-512; `addr` 4-byte aligned |
| `0x31 FLASH_ERASE` | `addr:4` | | One 4 KiB sector, at or above `upload_start` |
| `0x32 FLASH_WRITE` | `addr:4 data` | | 1-256 bytes within one page, at or above `upload_start` |
| `0x3E SET_LED` | `state:1` | | System LED: 0 off, 1 fast, 2 slow, 3 on |
| `0x3F REBOOT` | | | Replies, then resets the chip |

| Unsolicited | Payload |
|---|---|
| `0x40 BEACON` | hello, once a second while no host is present |
| `0x41 RX_FRAME` | the 8-byte NIC RX descriptor (port in byte 3, length in bytes 4-5), then the frame with its CPU tag |
| `0x42 LINK` | `links_sts:4 links:4 links_89:4` (registers 0x63e8, 0x63f0, 0x63f4) |
| `0x43 SFP` | `sfp_pins:1`, as `sfp_pins_last` in `sfp.c` |
| `0x44 LOG` | text the shared modules print |

The **hello** payload holds, in order:

- protocol version, 1 byte;
- chip ID, 4 bytes;
- MAC, 6 bytes: from flash at `machine.mac_flash_offset`, otherwise the derived locally administered
  address RTLPlayground uses;
- flash size, 4 bytes;
- image size, 4 bytes;
- upload start, 4 bytes;
- CPU port, minimum port, maximum port and SFP count, 1 byte each;
- flags, 1 byte: RTL8373, N variant, compact layout;
- machine name, 30 bytes;
- firmware version, the rest of the message.

Status codes: 0 ok, 1 bad length, 2 bad argument, 3 failed, 4 unknown request.

**Flash writes stay above the running image.** Writes are allowed only at or above `upload_start`,
which covers the staging area, the saved config sector and any MAC slot above the image. To update the
bridge, the RP2040 erases and writes a complete image at `upload_start`, reads it back to verify, then
sends `REBOOT`. The 8051 checks the CRC at boot and installs the image, as RTLPlayground does for a
web upload. Expect the same slow writes, one Page Program per 4 bytes: about 2-3 minutes for a
compact image on the W25Q40.

### Register ownership after `HELLO`

Several engines can be reached from both sides, so each has one owner.

| Engine | Owner | Host access |
|---|---|---|
| CPU-port NIC (0x7840-0x7890), interrupt status/mask (0x5f34, 0x5f84, 0x5f88, 0x7854) | 8051 | none |
| SerDes, PHY (MDIO), flash | 8051 (SFR only) | through requests |
| SoC I²C master (SFP EEPROMs), SFP GPIOs | 8051 | `SFP_READ` |
| System LED | either | `SET_LED`, or the register directly when the 8051 isn't using it |
| Everything else: VLAN, L2, ACL and MIB table engines, port, trap, mirror, storm and bandwidth registers | RP2040 | I²C on T3, or `REG_READ`/`REG_WRITE` |

The 8051 doesn't touch the RP2040's registers after boot, so the host may use either path.

- **Before `HELLO`.** The host must not write registers. The 8051 answers `HELLO` only after its
  initialisation has finished, because it starts serving requests only then.
- **One path at a time.** The table engines (`TBL_CTRL` and friends) take several register accesses
  per operation. The host must not interleave an I²C sequence and an RPC sequence on the same engine.

### Failure behaviour

| Event | Result |
|---|---|
| No RP2040, or it stops sending | After 3 s the bridge drops CPU-port frames and sends beacons. Forwarding continues with the registers as last written |
| 8051 stops answering | The RP2040 sees `PING` time out and pulses the reset line (if wired). The switch reboots to defaults, which interrupts traffic for a few seconds; the RP2040 then re-applies its configuration after `HELLO` |
| Corrupt or lost bytes on the UART | The message fails its CRC and is counted in `link_errors`. The host retries after its reply timeout |
| Bad staged image | The 8051 refuses it at boot (CRC) and erases the staging area |
| Bricked flash | A programmer (CH341A) on the SPI flash, as today; the mod chip doesn't change that |

## Mod chip hardware

### Electrical

- **MCU and flash.** RP2040 (QFN-56) with a 12 MHz crystal and 2 MiB QSPI flash (W25Q16JV), the same
  core circuit as the Raspberry Pi Pico.
- **Power from the switch.** The 3.3 V comes from T3 pin 4 and T8 pin 4, through an ideal diode
  (LM66100). The switch's TMI3244T rail is rated 4 A; the mod chip draws about 30-60 mA.
- **Power over USB.** The optional USB-C (development only) feeds an AP2112K-3.3 through a second
  LM66100. The two diodes keep USB from powering the whole switch, and the switch from back-feeding
  USB.
- **No back-powering through I/O.** A switch-side 3.3 V sense (divider to GP26/ADC0) gates the I/O. The
  RP2040 leaves every switch-facing pin high-impedance until the switch is powered. The 100 Ω series
  resistors limit current if that ever fails.
- **Development extras.** BOOTSEL and RUN buttons, SWD pads, and a status LED on GP25.
- **Board stack.** A 2-layer board works (the Pico is 2-layer); 4 layers make the RP2040 decoupling
  easier.

| RP2040 pin | Use |
|---|---|
| GP0 / GP1 | UART0 TX / RX → T5 |
| GP4 / GP5 | I2C0 SDA / SCL → T3 |
| GP6 / GP7 | T3 SPI DO / CS (spare) |
| GP8-GP11 | T8 GPIO46, 47, 48, 49 |
| GP12 | SoC reset driver (2N7002) |
| GP25 | status LED |
| GP26 | switch 3.3 V sense |

### Mechanical

All measurements here are *photo* estimates, in board coordinates: x from the left edge and y from the
rear edge, seen from the top with the ports at the front. The PCB is about 165 × 120 mm.

| Feature | x (mm) | y (mm) | Height |
|---|---|---|---|
| T3 pin 1 (square) | 40.2 | 3.5 | through-hole |
| T8 pin 6 | 70.6 | 3.5 | through-hole |
| J1 DC jack, right edge | 25.8 | 0-20 | tall (~11 mm) |
| L2/L3 inductors | 29-37 | 7-13 | ~2 mm |
| U4, U10 footprints | 34-48 | 10-30 | empty |
| C103 (MLCC), D3/R78 (reset) | 68-80 | 8-23 | ~1.5 mm |
| C124 electrolytic, left edge | 79.6 | 0-13 | tall (~11 mm) |
| Heatsink | 63-92 | from 23 | ~10-15 mm |
| U6/U8 SPI flash (SOIC-8) | 33-46 | 33-40 | 1.75 mm |
| T5, pins 1-4 | ~160 | 34-43 | through-hole |

**T3 and T8 share one grid.** T3 pin 1 to T8 pin 6 is 30.5 mm, exactly 12 × 2.54 mm. They form one
1×13 row with position 7 empty, so the mod chip uses one 13-position connector with pin 7 removed.

**Outline.** About **50 × 21 mm**, placed at x 28-78 and y 0.5-21.5. That stays inside J1, C124 and
the heatsink. The connector row lies along the mod chip's rear edge, 3 mm in from it. The only parts
under the board are low ones: L2/L3, C103, D3 and the empty U4/U10. Keep the mod chip's underside free
of components.

**Mounting** (rev A: removable):

- solder a low-profile 1×13 female socket (about 4.4 mm) into T3+T8 on the switch;
- the mod chip carries matching male pins pointing down;
- stack height above the switch PCB: socket 4.4 + PCB 1.0 + parts ≤ 3.2 mm, about 8.6 mm in all. That
  is below the electrolytics and the heatsink, which the lid already has to clear.
- Rev B could solder the mod chip straight onto a 2.5 mm pin-strip spacer for about 6.7 mm.

**Harness to T5.** About 120 mm, routed along the rear edge.

**Reset wire.** From the mod chip's reset pad (near T8) to T-D3, about 10 mm.

**USB-C.** On the mod chip's left edge, for bench work with the lid off. Units in service update over
the network.

**Measure before laying out:**

- the lid's inside height above T3/T8 and above C124;
- the gap between the PCB's rear edge and the chassis wall;
- the T3 and T8 positions and pitch, on both switches;
- whether T5 has pins fitted.

## RP2040 firmware (outline)

- **Core 1 (link).** UART RX by DMA into a ring, with a deframer. A request queue enforces one request
  in flight, with a reply timeout of 100 ms plus the transfer time. I²C register access runs on T3.
  Events go to core 0 through a FIFO.
- **Core 0.** lwIP on a virtual netif:
  - **Receive.** An `RX_FRAME` has its CPU tag stripped, which also gives the ingress port, before it
    goes to lwIP.
  - **Transmit.** Output frames go out with `TX_FRAME`; protocol frames get a CPU tag to pick the
    egress port.
  - **Applications.** A web UI, which can reuse RTLPlayground's `html/` pages and JSON endpoints, plus
    STP, LACP, IGMP and LLDP. The latter can come from RTLPlayground's C through a register shim.
  - **Configuration.** LittleFS in the RP2040's flash, applied after each `HELLO`.
- **Updates.**
  - RP2040 firmware: over its web UI, with an A/B or bootloader scheme, or by USB.
  - Bridge firmware: through `FLASH_WRITE` staging, as above.

## Bring-up plan

1. **Chip ID.** Run `pico/read_chip_id.py` against T3 with RTLPlayground still running. Expect
   `0x8372xx00` and a ticking seconds counter.
2. **Writes.** Confirm the T3 write format on `RTL837X_REG_SEC_COUNTER`, then raise the I²C clock to
   400 kHz and 1 MHz.
3. **Check the headers with a meter.** T3, T5 and T8 pin 4 at 3.3 V; idle levels on T8.
4. **Flash the bridge.** On the MokerLink, write
   `bridge-…-SWGT024_V2_0_UNMANAGED-compact-flash512k.bin` with the CH341A. Keep the stock and
   RTLPlayground recovery images at hand: the bridge has no web UI.
5. **Talk to it.** Run `pico/bridge_host_test.py` against T5. Expect:
   - beacons and boot log lines;
   - `HELLO` answered;
   - `LINK` events when cables are plugged in;
   - `RX_FRAME`s for broadcast traffic (ARP).
6. **Transmit.** Send a test frame with `TX_FRAME` and capture it on a PC.
7. **Faster baud.** Try `LINK_TH1=0xff` (1 953 125 baud) and watch `link_errors` and `rx_dropped`.
8. **Reset line.** Find T-D3 and check the reset pulse from GP12 using a Pico on jumper wires.
9. **Order hardware.** Lay out the mod chip PCB only once steps 1-8 pass and the measurements above
   are confirmed.
