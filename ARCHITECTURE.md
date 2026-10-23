# Mod chip: an RP2350 management processor for SWTG024AS-V2.0 switches

Status: design draft. The firmware builds and its link framing is tested on the host, but none of this
has run on hardware yet. Measurements marked *photo* come from the board photo in
RTLPlayground's `doc/devices/photos/SWTG024AS-v2.0-unmanaged/` and must be checked with calipers before a PCB is
ordered.

## Goal

Move the switch's management plane off the RTL8372's built-in 8051 onto an RP2350 on a small board
inside the chassis. The 8051 keeps only what has to run on it. Target boards: the SWTG024AS-V2.0
family, and in particular the two on hand:

| Switch | PCB | Flash | Bridge build |
|---|---|---|---|
| MokerLink 2G040210GS | SWTG024AS-V2.0, unmanaged layout | W25Q40, 512 KiB | `MACHINE=SWGT024_V2_0_UNMANAGED LAYOUT=compact` |
| XikeStor SKS3200M-4GPY2XF | SWTG024AS-V2.0, managed layout | FM25Q16, 2 MiB | `MACHINE=SWGT024_V2_0_MANAGED` |

[`doc/devices/SWTG024AS.md`](https://github.com/logicog/RTLPlayground/blob/main/doc/devices/SWTG024AS.md) lists the XikeStor as a v1.0 PCB, but the board on hand is marked v2.0.
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
chip, keeps the SerDes and SFPs working, and tunnels frames and SFR-only operations to the RP2350 over
its UART. The RP2350 does everything else.

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
                         │                    │        │  rigid tail
 ┌───────────────────────┴────────────────────┴────────┴────────────────────────┐
 │ mod chip: RP2350A, 16 MiB QSPI flash, ideal diode, reset driver, SWD pads    │
 │  main loop: link driver (UART, COBS/CRC), register access by RPC,            │
 │  IP stack, web UI, STP/LACP/IGMP/LLDP, configuration                         │
 └──────────────────────────────────────────────────────────────────────────────┘
```

### What runs where

| Function | 8051 (bridge) | RP2350 |
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
| Console | | ✓ (over the network; SWD/RTT on the bench) |

The 8051 sets up the same forwarding defaults RTLPlayground starts from. With no RP2350 present, or
with one that has crashed, the switch therefore still forwards as an unmanaged switch. Registers the
RP2350 has written stay in effect until the next reset.

RTLPlayground's protocol code (`rtl837x_port.c`, `rtl837x_stp.c`, `rtl837x_lacp.c`,
`rtl837x_igmp.c`, …) talks to the chip only through `reg_read_m()`/`reg_write_m()`, `sfr_data` and
`phy_*()`. A shim that maps those onto I²C and bridge requests would let much of it compile for the
RP2350 largely unchanged. `test/sdcc_shim.h` already does the equivalent for the host tests.

### Why the RP2350

It was chosen over the RP2040 mainly for HTTPS:

- **Speed.** Two Cortex-M33 cores at 150 MHz, with DSP instructions.
- **Memory.** 520 KB of SRAM, enough for mbedTLS with full 16 KB records alongside lwIP and the web UI.
- **Randomness.** A hardware random number generator, for generating TLS keys on the device. The RP2040
  has none.
- **Hashing.** A SHA-256 accelerator.
- **Keys.** OTP memory, where a device key or a secure-boot key can live.

A rough estimate, from published Cortex-M benchmarks rather than measurement on this design:

- **New connection.** A TLS 1.3 handshake with X25519 and an ECDSA P-256 certificate should cost well
  under a second.
- **Bulk data.** ChaCha20-Poly1305 runs at several MB/s.
- **Bottleneck.** The UART link to the switch, about 97 KB/s, limits page loads, not the cryptography.

## Interfaces

### T3: slave interface (register access)

| T3 pin | Signal | Mod chip |
|---|---|---|
| 1 (square) | SCL / SCK / MDC | RP2350 GP11 (I2C1 SCL), 100 Ω series |
| 2 | GND | GND |
| 3 | SDA / SDI / MDIO | RP2350 GP10 (I2C1 SDA), 100 Ω series |
| 4 | VCC (3.3 V, verify) | power in, through an ideal diode |
| 5 | SPI DO (through 33 Ω) | GP9, for SPI-mode experiments only |
| 6 | SPI CS | GP8, likewise |

- **Mode and address.** The `IF_SEL` straps put this board in I²C mode at 0x5C. The RP2350 has no pin
  that is both an I²C SCL and an SPI SCK, so SPI mode would have to run on PIO. That's not needed for
  this board.
- **Register format.** The address is 16 bits, high byte first; data is 32 bits, low byte first (see
  `doc/devices/SWTG024AS.md` in RTLPlayground). `firmware/micropython/read_chip_id.py` checks the read path.
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
| 1 (square) | U0TXD, switch output | RP2350 GP1 (UART0 RX) |
| 2 | GND | GND |
| 3 | U0RXD, switch input | RP2350 GP0 (UART0 TX), 100 Ω series |
| 4 | 3.3 V | not connected |

- **Wiring.** T5 is about 90 mm from T3 (*photo*). The board outline reaches it with a rigid tail; see
  Mechanical. A 3-wire harness is the fallback if the tail's fit turns out to be impractical.
- **Baud rate.** The bridge runs UART0 at 976 562 baud (`LINK_TH1 0xfe`), which the 125 MHz 8051
  generates exactly.
- **RP2350 side.** From its default 150 MHz `clk_peri`, the RP2350's fractional divider comes within
  0.07 % of that rate, well inside UART tolerance. Running `clk_sys` at 125 MHz instead makes it exact.
- **Faster option.** 1 953 125 baud (`0xff`; again within 0.07 % on the RP2350) is the next step, if the 8051 keeps up
  with its per-byte CRC and COBS work. Measure it on hardware.
- **Throughput.** About 97 KB/s each way at 976 562 baud. A 1500-byte frame takes 15 ms.

### T8: SoC GPIOs (sideband, optional)

| T8 pin | Signal | Mod chip |
|---|---|---|
| 1 (square) | GPIO46 (I2C_SCL0) | GP7, 100 Ω |
| 2 | GND | GND |
| 3 | GPIO48 (I2C_SCL1) | GP6, 100 Ω |
| 4 | 3.3 V | power in, through the same ideal diode |
| 5 | GPIO47 (I2C_SDA0) | GP5, 100 Ω |
| 6 | GPIO49 (I2C_SDA1) | GP4, 100 Ω |

These pins are the SoC's I²C master buses 0 and 1, unused on this board. Protocol v1 doesn't use them.
They're wired because they cost nothing: T8 sits in the same row as T3, on the same 2.0 mm pitch
with one empty position between them.

Planned uses:

- an 8051 → RP2350 "attention" line, to cut latency without polling;
- an RP2350 → 8051 strap read at boot, for example to start RTLPlayground instead of the bridge for
  recovery;
- possibly a second data channel, with the SoC's hardware I²C master talking to the RP2350 as an I²C
  slave.

Their default states and any pull-ups on the switch side are unverified.

### Reset (optional fly-wire)

The SoC reset line is active low. It is an RC network: R78 (33 kΩ) pulls it up to 3.3 V, C187 holds
it low at power-on, and D3 discharges C187 when the power drops. D3 is a glass MiniMELF about 13 mm
in front of T8 (*photo*), under the mod chip's main section.

**Which end of D3.** The reset node is D3's **anode**:

- it is the **front** pad (y ≈ 17.9 mm), the end beside the "+" printed on the board and away from the
  black band;
- the banded rear end is the cathode, which in this circuit sits on the 3.3 V rail.

Wiring TP1 to the cathode and pulling it low would short the switch's 3.3 V supply. Check with a meter
before soldering, with the switch unpowered:

- the cathode reads about 0 Ω to T5 pin 4 (3.3 V);
- the anode reads about 33 kΩ, through R78.

**Driver.** The mod chip pulls the line low through a 2N7002 from GP27, with **1 kΩ in series** between
the drain and TP1. That still takes the node to about 0.1 V against R78. If the wire ever lands on the
3.3 V rail, it limits the current to 3.3 mA instead of shorting the supply. This lets the RP2350 restart
a hung 8051 without a power cycle. Without the wire, everything else still works.

## Bridge firmware (8051)

The source lives in RTLPlayground, [`modchip/bridge/`](https://github.com/wrcrooks/RTLPlayground/tree/feat/modchip/modchip/bridge) on the `feat/modchip` branch of the fork,
because it shares the chip-level modules there. Build it from that repository's root:

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
without any credit scheme. The RP2350 receives into a DMA ring and needs no flow control. If the ISR
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
bridge, the RP2350 erases and writes a complete image at `upload_start`, reads it back to verify, then
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
| Everything else: VLAN, L2, ACL and MIB table engines, port, trap, mirror, storm and bandwidth registers | RP2350 | I²C on T3, or `REG_READ`/`REG_WRITE` |

The 8051 doesn't touch the RP2350's registers after boot, so the host may use either path.

- **Before `HELLO`.** The host must not write registers. The 8051 answers `HELLO` only after its
  initialisation has finished, because it starts serving requests only then.
- **One path at a time.** The table engines (`TBL_CTRL` and friends) take several register accesses
  per operation. The host must not interleave an I²C sequence and an RPC sequence on the same engine.

### Failure behaviour

| Event | Result |
|---|---|
| No RP2350, or it stops sending | After 3 s the bridge drops CPU-port frames and sends beacons. Forwarding continues with the registers as last written |
| 8051 stops answering | The RP2350 sees `PING` time out and pulses the reset line (if wired). The switch reboots to defaults, which interrupts traffic for a few seconds; the RP2350 then re-applies its configuration after `HELLO` |
| Corrupt or lost bytes on the UART | The message fails its CRC and is counted in `link_errors`. The host retries after its reply timeout |
| Bad staged image | The 8051 refuses it at boot (CRC) and erases the staging area |
| Bricked flash | A programmer (CH341A) on the SPI flash, as today; the mod chip doesn't change that |

## Mod chip hardware

### Electrical

The full circuit is in [`hardware/modchip.kicad_sch`](hardware/modchip.kicad_sch)
([PDF](hardware/schematic.pdf)), generated and checked as described in
[`hardware/README.md`](hardware/README.md).


- **MCU and flash.** An RP2350A (QFN-60, 7 × 7 mm) with a W25Q128JVSIQ, 16 MiB of QSPI flash in a
  208-mil SOIC-8 (U5). That's the default build, and the flash is a JLCPCB Basic part.
- **RP2354A variant.** The board also takes an RP2354A, an RP2350A with 2 MiB of flash inside the
  package, same footprint and pinout. Leave U5 and its capacitor C19 off: the RP2354A's internal
  flash sits on the same QSPI pads, so fitting both would put two chips on one chip select.
- **Crystal.** 12 MHz, the Abracon ABM8-272-T3, which the RP2350 is tuned for.
- **Core regulator.** The RP2350's core supply comes from an on-chip switching regulator: a 3.3 µH
  inductor on VREG_LX and an RC filter on VREG_AVDD, laid out after the Pico 2 (see Layout below).
- **Power.** The 3.3 V comes from T3 pin 4 and T8 pin 4, through an LM66100 ideal diode. The switch's
  TMI3244T rail is rated 4 A; the mod chip draws about 30-60 mA.
- **Bench power and debug.** A debug probe on J5 (+3V3, SWCLK, GND, SWDIO, RUN) programs and debugs
  the RP2350 and can power the board on the bench. The ideal diode keeps that 3.3 V out of the
  switch. There's no USB port: no plug fits inside the switch, and SWD covers programming.
- **No back-powering through I/O.** A switch-side 3.3 V sense (divider to GP26/ADC0) gates the I/O. The
  RP2350 leaves every switch-facing pin high-impedance, with its input buffer off, until the switch is
  powered. The 100 Ω series resistors limit current if that ever fails.
- **Pull-downs.** Don't rely on the RP2350's internal pull-downs. Erratum RP2350-E9 makes them latch
  on early silicon. The design uses only external resistors, so it isn't affected.
- **Status LED.** On GP22.

| RP2350 pin | Use |
|---|---|
| GP0 / GP1 | UART0 TX / RX → T5 |
| GP11 / GP10 | I2C1 SCL / SDA → T3 pins 1 / 3 |
| GP9 / GP8 | T3 SPI DO / CS (spare) |
| GP7, GP6, GP5, GP4 | T8 GPIO46, GPIO48, GPIO47, GPIO49 |
| GP27 | SoC reset driver (2N7002, 1 kΩ series to TP1) |
| GP22 | status LED |
| GP26 | switch 3.3 V sense (ADC0) |

The GPIOs follow the board, not the other way round. With U1 rotated 270°, GP11…GP0 run left to right
along the side facing T3 and T8, in the order of the header pins they reach, so no two routes cross.

### Layout

- **Stack.** Four layers: F.Cu parts and signals, In1 GND plane, In2 3.3 V plane, B.Cu signals.
  Nothing but the headers on the bottom side.
- **Why not two layers.** The RP2350 doesn't need four; the Pico 2 is two layers. This board puts a
  0.4 mm-pitch QFN-60 and everything around it on one side of about 39 × 17 mm, and two inner planes
  are what make it route. They also give the regulator unbroken ground and feed the supply pins
  without tracks. At JLCPCB, four layers cost a few dollars more per batch.
- **Hand-routed, before the router runs:**
  - VREG_LX straight to L1, and VREG_AVDD to its filter capacitor;
  - U1's right-side 3.3 V pins (VIN, USB_OTP_VDD, QSPI_IOVDD) on a short trunk to CIN;
  - +1V1: each DVDD pin (6, 23, 39) and VREG_FB (50) drops a via just inside the pad row, and B.Cu
    tracks round the exposed pad join them. Once the signals have escaped, those pins are boxed in;
  - the fan of T8 lines and the UART RX lane above U1, stacked so no two cross;
  - UART TX straight down to B.Cu, and pin 20's and pin 38's plane vias, placed out of their
    neighbours' way.
- **Autorouted.** Everything else is Freerouting plus kicad-agent-kit's A* finisher.
- **Escape corridors.** Auto-placed parts are kept out of corridors on all four sides of U1.
- **Result.** KiCad DRC is clean: no errors, no warnings, schematic parity included.
- **Tracks and vias.** 0.15 mm signal tracks (0.4 mm pitch leaves 0.225 mm to the next pad), 0.3 mm
  power tracks, 0.5/0.2 mm vias, and JLCPCB's design rules (`modchip.kicad_dru`).

### Parts and assembly (JLCPCB)

Every part has an LCSC number in the schematic. JLCPCB Basic parts wherever one exists: every resistor
and capacitor, the 2N7002, the W25Q128JVSIQ and the LED (red 0603: JLC's Basic LEDs are red or white).
Extended parts, all in stock:

| Part | LCSC | Why not Basic |
|---|---|---|
| RP2350A | C42411118 | none exists |
| LM66100 | C2869734 | none exists |
| AOTA-B201610S3R3-101-T (L1) | C42411119 | the datasheet's inductor; no Basic 3.3 µH 0806 |
| ABM8-272-T3 (Y1) | C20625731 | the Basic 12 MHz crystal (C9002) has a 20 pF load and 80 Ω ESR; the RP2350 is tuned for 10 pF and at most 50 Ω |

**Not assembled.** The headers J1-J3, through-hole on the bottom side, are soldered by hand to match the
sockets fitted on the switch, and stay out of the assembly BOM. If wanted: C22438289 (2.0 mm 1×6) and
C32713270 (2.54 mm 1×4). The SWD pads (J5) and the reset pad (TP1) are bare copper.

### Mechanical

The outline is generated and checked by [`hardware/outline.py`](hardware/outline.py), which also writes
a DXF for KiCad and a 1:1 print template; see [`hardware/README.md`](hardware/README.md). All positions
are *photo* estimates, in board coordinates: x from the left edge and y from the rear edge, seen from the
top with the ports at the front.

**Source.** The positions are measured on up-n-atom's photo of a managed board, which is close to a
flat scan, at 38.3 px/mm. That's calibrated on U8's 1.27 mm and T3's 2.0 mm pin pitches; expect about
±0.3 mm. On that scale:

- the PCB is about 130.3 × 88.7 mm;
- T3 and T8 are 2.0 mm pitch: T3 pin 1 to T8 pin 6 is 24.0 mm;
- T5 is 2.54 mm.

| Feature | x (mm) | y (mm) | Notes |
|---|---|---|---|
| T3 pin 1 (square) | 32.4 | 1.95 | 2.0 mm pitch, 6 pins |
| T8 pins 1-6 | 46.4-56.4 | 1.95 | same row; one empty position after T3 |
| T5 pins 1-4 | 126.6 | 26.85-34.5 | 2.54 mm pitch; pin 1 (TX) at the rear |
| D3 (reset line) | 59.4-61.0 | 12.8-17.7 | |
| U8 SPI flash, 208-mil SOIC-8 | body 28.3-33.5 | 24.5-29.8 | leads 26.2-36.0 |
| J1 DC jack, right edge | 21.8 | to 12.9 | THT |
| C124, C127 electrolytics | 63.1-80.8 | to 10.2 | tall |
| 1R5 power inductor | 81.3-94.3 | 1.9-16.4 | tall SMD |
| Heatsink | 50.1-71.2 | 19.1-40.1 | tall |
| C102, EC1 electrolytics | centres 119.4 | 12.5, 21.5 | THT, about 9 mm |
| Mounting standoffs, rear | 4.5 and 126.3 | 11 | |
| SFP cages J4, J2 | 81.9-126.6 | from 43.5 | THT |

**Outline.** One rigid board, about 106 × 37 mm overall, reaches T3, T8, T5 and D3:

- **Main section** (x 23.1-61.8, y 0.4-17.8), over T3+T8, between J1 and C124, behind the heatsink.
- **Neck** (y 11.5-17.8) under C124 and C127.
- **Strip** (y 17.7-24.5) under the 1R5 inductor and past the heatsink's right side.
- **Tail** (x 101-129.3, y 25.3-37.6) that steps around EC1 to reach T5 and stops short of the J2 cage.

It keeps at least 1.3 mm from every through-hole or tall part, and 2.7 mm from the test-clip zone
around U8. That zone reaches 4 mm beyond U8's leads and body, so the clip itself has more room. Only
SMD parts under 3 mm sit beneath the board (L2/L3, D2, Q1/Q2, small R/C), so the mod chip's underside
carries no components.

**Mounting** (rev A: removable):

- solder low-profile female sockets into T3 and T8 (2.0 mm, 1×6 each) and T5 (2.54 mm, 1×4), all at
  the same height, about 4.5 mm;
- the mod chip carries matching male pins pointing down;
- stack height above the switch PCB: 4.5 + PCB 1.0 + parts ≤ 3.2 mm, about 8.7 mm in all. That is below
  the electrolytics, the inductor and the heatsink, which the lid already has to clear.
- Rev B could solder the mod chip straight onto pin strips for about 6.7 mm.

With two connectors about 90 mm apart, their relative position has to be right to a few tenths of a
millimetre. Check it with the 1:1 print and calipers before ordering. The board is supported only at
its connectors, so a 4.5 mm adhesive foot under the strip (beside D2) helps against flex.

**Reset.** TP1 sits above D3. Its wire goes to D3's anode pad, the front one, which lies just outside
the board's edge, about 2 mm away.

**Measure before laying out:**

- the lid's inside height above T3/T8 and above C124;
- the gap between the PCB's rear edge and the chassis wall;
- the T3, T8 and T5 positions and pitches (photo: 2.0, 2.0 and 2.54 mm), on both switches;
- whether T5 has pins fitted.

## RP2350 firmware

Written, in `firmware/rp2350/` (its README has the details); not yet run on hardware. It differs
from the first outline as follows:

- **One core.** Everything runs from one main loop, as on the 8051. Register access over the bridge
  blocks until the reply. The UART is drained into a ring by interrupt, and frames that arrive during
  a request are queued.
- **Register access.** Over the UART only, for now. I²C on T3 can follow once its write format is
  confirmed.
- **Configuration.** RTLPlayground's text config in one raw 4 KiB flash sector rather than LittleFS.
  It is replayed after each `HELLO`.
- **Bridge requests.** Four were added for the firmware: `TBL_READ` (64 table reads per request),
  `STAT_READ` (MIB counters of all ports in one request), `SDS_CONFIG` and `FLASH_ID`.
- **HTTPS and SNMP.** TLS 1.2 on port 443 with a key made on the device, and a read-only SNMPv2c
  agent listing the switch ports.
- **Graphs.** A dashboard page with live charts and an hour of history kept in RAM.
- **Updates.** RP2350 firmware by SWD for now. Bridge updates through `FLASH_WRITE` staging aren't
  wired to the web UI yet.

## Bring-up plan

1. **Chip ID.** Run `firmware/micropython/read_chip_id.py` against T3 with RTLPlayground still running. Expect
   `0x8372xx00` and a ticking seconds counter.
2. **Writes.** Confirm the T3 write format on `RTL837X_REG_SEC_COUNTER`, then raise the I²C clock to
   400 kHz and 1 MHz.
3. **Check the headers with a meter.** T3, T5 and T8 pin 4 at 3.3 V; idle levels on T8.
4. **Flash the bridge.** On the MokerLink, write
   `bridge-…-SWGT024_V2_0_UNMANAGED-compact-flash512k.bin` with the CH341A. Keep the stock and
   RTLPlayground recovery images at hand: the bridge has no web UI.
5. **Talk to it.** Run `firmware/micropython/bridge_host_test.py` against T5. Expect:
   - beacons and boot log lines;
   - `HELLO` answered;
   - `LINK` events when cables are plugged in;
   - `RX_FRAME`s for broadcast traffic (ARP).
6. **Transmit.** Send a test frame with `TX_FRAME` and capture it on a PC.
7. **Faster baud.** Try `LINK_TH1=0xff` (1 953 125 baud) and watch `link_errors` and `rx_dropped`.
8. **Reset line.** Confirm D3's anode with a meter (about 33 kΩ to 3.3 V; the cathode reads 0 Ω). Then
   check a reset pulse through 1 kΩ and a 2N7002 from a Pico on jumper wires.
9. **Order hardware.** Lay out the mod chip PCB only once steps 1-8 pass and the measurements above
   are confirmed.
