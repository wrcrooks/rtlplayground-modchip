# Read the RTL8372/RTL8373 chip ID through the slave management interface.
#
# MicroPython for a Raspberry Pi Pico or Pico 2. Only reads registers; it never
# writes to the switch.
#
# Wiring, Pico -> SWTG024AS-V2.0 header T3 (pin 1 is the square pad):
#
#   Pico GP4  (pin 6, I2C0 SDA)  ->  T3 pin 3  (SDA)
#   Pico GP5  (pin 7, I2C0 SCL)  ->  T3 pin 1  (SCL)
#   Pico GND  (pin 8)            ->  T3 pin 2  (GND)
#
# Leave T3 pin 4 (the switch's 3.3 V) unconnected and power the Pico from
# USB. Check with a meter first that T3 pin 4 reads 3.3 V and not 5 V: the
# Pico's pins are not 5 V tolerant.
#
# Run it with:  mpremote run read_chip_id.py
#
# Protocol, from doc/devices/SWTG024AS.md: a 16-bit register address, high
# byte first, then a repeated start and four data bytes, low byte first.

from machine import I2C, Pin
import time

SLAVE_ADDR = 0x5C
FREQ = 100_000          # the bus has no known pull-ups; keep it slow at first

REG_CHIP_ID = 0x0004
REG_CHIP_UUID = 0x0010
REG_SEC_COUNTER = 0x06F4
REG_LINKS_STS = 0x63E8
REG_LINKS = 0x63F0
REG_LINKS_89 = 0x63F4

# SWTG024AS-V2.0: front-panel name -> switch MAC (port) number
PORTS = (("RJ45 1", 4), ("RJ45 2", 5), ("RJ45 3", 6), ("RJ45 4", 7),
         ("SFP J4", 8), ("SFP J2", 3))
SPEEDS = {0: "10M", 1: "100M", 2: "1G", 4: "10G", 5: "2.5G", 6: "5G"}


def reg_read(i2c, reg):
    """One 32-bit switch register. readfrom_mem with addrsize=16 sends the
    address high byte first, then a repeated start before the read."""
    data = i2c.readfrom_mem(SLAVE_ADDR, reg, 4, addrsize=16)
    return data[0] | data[1] << 8 | data[2] << 16 | data[3] << 24


def link_state(i2c, port):
    sts = reg_read(i2c, REG_LINKS_STS)
    # Same bit positions as port_stats_print() in rtl837x_port.c
    if not (sts >> (16 + port if port < 8 else port)) & 1:
        return "down"
    if port < 8:
        code = reg_read(i2c, REG_LINKS) >> (4 * port) & 0xF
    else:
        code = reg_read(i2c, REG_LINKS_89) >> (4 * (port - 8)) & 0xF
    return SPEEDS.get(code, "up (code %d)" % code)


def main():
    # MicroPython's rp2 port enables the pins' internal pull-ups (about 50 kOhm),
    # which is enough at 100 kHz over a short wire.
    i2c = I2C(0, sda=Pin(4), scl=Pin(5), freq=FREQ)

    found = i2c.scan()
    print("I2C scan:", ["0x%02x" % a for a in found] or "nothing")
    if SLAVE_ADDR not in found:
        print("No device at 0x%02x. Check:" % SLAVE_ADDR)
        print(" - the switch is powered and has finished booting;")
        print(" - SDA and SCL are not swapped (T3 pin 1 is SCL, pin 3 SDA);")
        print(" - GND is connected;")
        print(" - pull-ups: add 4.7k from SDA and SCL to the Pico's 3V3 (pin 36);")
        print(" - the IF_SEL straps on this board may select SPI or MDIO instead of I2C.")
        return

    chip_id = reg_read(i2c, REG_CHIP_ID)
    print("Chip ID register 0x0004: 0x%08x" % chip_id)
    model = chip_id >> 16
    if model in (0x8372, 0x8373):
        variant = "N" if (chip_id >> 8) & 0xFF == 0x70 else ""
        print("  -> RTL%04x%s" % (model, variant))
    else:
        print("  -> unexpected; an RTL8372 reads 0x8372xx00")
        return

    print("Chip UUID: 0x%08x" % reg_read(i2c, REG_CHIP_UUID))

    # Proof that the switch is live: the firmware's seconds counter ticks
    s1 = reg_read(i2c, REG_SEC_COUNTER)
    time.sleep(2.2)
    s2 = reg_read(i2c, REG_SEC_COUNTER)
    print("Seconds counter: %d -> %d after 2.2 s" % (s1, s2))

    print("Links:")
    for name, port in PORTS:
        print("  %-7s (port %d): %s" % (name, port, link_state(i2c, port)))


main()
