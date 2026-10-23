# Testing with a Beetle RP2350

A DFRobot [Beetle RP2350](https://wiki.dfrobot.com/DFR1188) (DFR1188) can stand in for the mod chip
until the boards arrive. It has the same RP2350A and 520 KB of RAM, so it runs the full firmware,
HTTPS included. It brings out the same GPIOs the mod chip uses for the bridge UART, the 3.3 V sense
and the reset driver.

**What this tests:**
- the bridge firmware and its protocol on real hardware;
- register, PHY, SerDes, SFP and flash access;
- CPU-port frames both ways, IP, DHCP and ARP;
- the web UI, `/cmd`, HTTPS, SNMP and the Graphs dashboard;
- STP, LACP, IGMP and LLDP on real links;
- config replay and SFP diagnostics.

**What it doesn't:**
- the mod chip board itself: the ideal diode, its reset driver, its flash and crystal;
- its fit on the headers;
- register access over T3, which the firmware doesn't use yet.

## Parts

- Beetle RP2350, USB-C cable
- 1 × 100 Ω resistor (series in the Beetle → switch UART line)
- 2 × 100 kΩ resistors (3.3 V sense divider)
- optional, for the reset line: a 2N7002 (or a 2N3904 with a 4.7 kΩ base resistor), 1 kΩ, 100 kΩ
- thin wire, or pin headers on the switch's T3/T5

## Wiring

**Wire everything with the switch unplugged. Never clip onto or probe a powered board.**

| Beetle pad (DFRobot label) | GPIO | Goes to | Notes |
|---|---|---|---|
| 0 (D0) | GP0, UART0 TX | T5 pin 3 (8051 RX) | through 100 Ω |
| 1 (D1) | GP1, UART0 RX | T5 pin 1, square pad (8051 TX) | direct |
| GND | — | T5 pin 2 (GND) | the only power connection between the boards |
| 26 (A0) | GP26, ADC0 | middle of a 100k/100k divider from T3 pin 4 (switch 3.3 V) to GND | the switch-powered sense |
| 27 (A1) | GP27 | reset driver, below | optional |
| 4 (D4) | GP4, I2C0 SDA | T3 pin 3 (SDA) | only for `read_chip_id.py` |
| 5 (D5) | GP5, I2C0 SCL | T3 pin 1, square pad (SCL) | only for `read_chip_id.py` |

```
T5 pin 1 (8051 TX) ─────────────────────── GP1  (Beetle pad 1)
T5 pin 3 (8051 RX) ────[100 Ω]──────────── GP0  (Beetle pad 0)
T5 pin 2 (GND) ─────────────────────────── GND

T3 pin 4 (3.3 V) ──[100k]──┬──[100k]── GND
                           └────────────── GP26 (Beetle pad 26)
```

**Power the Beetle from USB, not from the switch.** Don't connect the switch's 3.3 V to the Beetle's
3V3 pad. The firmware keeps every switch-facing pin isolated until GP26 reads the switch's 3.3 V, so
a Beetle powered while the switch is off doesn't back-feed it. The divider gives it that reading;
on the mod chip, the ideal diode and the same sense input do that job.

UART0 on GP0/GP1 is what the firmware uses. DFRobot's wiki calls those pads "UART1"; that's their
numbering, not the chip's.

### The reset line (optional)

The firmware pulses the switch's reset only if it has seen the bridge and then lost it (three missed
pings, at most once every 30 s). With nothing on GP27, that just doesn't happen. To wire it:

```
GP27 ──────────┬──── 2N7002 gate
              [100k]                  drain ──[1 kΩ]── TP1 (D3's anode)
GND ───────────┴──── 2N7002 source
```

GP27 high turns the transistor on and pulls the reset node low through 1 kΩ. The 100 kΩ keeps the
transistor off while the Beetle boots.

**Use D3's anode, never its cathode.** D3 is the glass MiniMELF diode near the reset circuit. Its
anode is the front pad, next to the "+". The banded cathode sits on the switch's 3.3 V, and pulling
it low would short the supply. Check with a meter before soldering: the anode reads about 33 kΩ to
3.3 V, the cathode 0 Ω. Never wire a GPIO straight to TP1.

## The switch side: flash the bridge firmware

The Beetle talks to the 8051 **bridge firmware**, not to RTLPlayground. While the bridge runs, the
switch has no web UI of its own; it keeps forwarding with the default setup.

1. **Keep recovery images at hand:** the stock dump and an RTLPlayground image for your switch.
2. **Build the bridge** in RTLPlayground, branch `feat/modchip`, from that repository's root.
   - MokerLink, 512 KiB flash:
     `make -f modchip/bridge/Makefile MACHINE=SWGT024_V2_0_UNMANAGED LAYOUT=compact`
   - XikeStor, 2 MiB flash: `make -f modchip/bridge/Makefile MACHINE=SWGT024_V2_0_MANAGED`
3. **Flash it with the CH341A**, switch unpowered. On the MokerLink, write the `…-flash512k.bin`
   image, which covers the whole chip.

## Step 1 (optional): MicroPython

Before the C firmware, MicroPython for the RP2350 on the Beetle can confirm the wiring with the
tools in `firmware/micropython/`. Both already use the right pins.

- **`bridge_host_test.py`** (UART0 on GP0/GP1). Expect beacons and boot log lines, then an answered
  `HELLO`, then `LINK` events as you plug cables in, then received frames for broadcast traffic.
- **`read_chip_id.py`** (I2C0 on GP4/GP5). Reads the chip ID over T3. This works with any firmware
  on the switch, RTLPlayground or stock.

## Step 2: the firmware

**Build.** With the Pico SDK 2.x, from the repository root:

```
git submodule update --init
cmake -S firmware/rp2350 -B firmware/rp2350/build-beetle -G Ninja \
      -DPICO_SDK_PATH=/path/to/pico-sdk -DMODCHIP_BOARD=beetle
ninja -C firmware/rp2350/build-beetle
```

Add `-DRTLP_MACHINE=SWGT024_V2_0_MANAGED` for the XikeStor.

**Flash.** Hold the Beetle's BOOT button while plugging in USB. Copy
`firmware/rp2350/build-beetle/modchip.uf2` to the `RP2350` drive that appears.

**Console.** The Beetle shows up as a USB serial port: `picocom /dev/ttyACM0`, or any terminal.
It takes RTLPlayground's commands, and lines from the 8051 show up prefixed `[8051]`.

**Power up the switch.** The console should show:

1. `Switch powered, starting the bridge link`. If it doesn't, check the divider on GP26.
2. `bridge: SWGT024 V2.0 …, chip 8372…, bridge firmware …`: the `HELLO` handshake.
   - Nothing at all: TX and RX swapped, or the bridge isn't flashed.
   - `link_errors` climbing on the dashboard's Bridge Link card: a bad connection or a missing
     ground.
3. `MAC: …`, then the config replay.

**Web UI.** The default config is RTLPlayground's: 192.168.10.247, password `1234`. The switch
answers at that address, served by the Beetle.
- **Graphs:** in the menu.
- **HTTPS:** `https on` in the command box, then https://192.168.10.247.
- **SNMP:** `snmp on`, then `snmpwalk -v2c -c public 192.168.10.247 1.3.6.1.2.1.2`.

## Things to note on the bench

- **Stray USB power.** The Beetle's USB power and the switch share only ground, so unplugging the
  switch leaves the Beetle running. The console then shows `Switch unpowered, releasing its pins`.
- **Saved settings.** The firmware keeps the config and the HTTPS key in the top two sectors of the
  Beetle's 2 MiB flash. Reflashing the firmware keeps them; a full erase doesn't.
- **Going back.** RTLPlayground's recovery images bring the stock web UI back. Flash one with the
  CH341A when you're done.
