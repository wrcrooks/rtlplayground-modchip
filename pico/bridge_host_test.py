# Talk to the mod-chip bridge firmware from a Raspberry Pi Pico.
#
# MicroPython. Copy bridge_link.py to the Pico first, then:
#   mpremote cp bridge_link.py :  &&  mpremote run bridge_host_test.py
#
# The switch must run the bridge firmware (modchip/bridge), not RTLPlayground.
#
# Wiring, Pico -> SWTG024AS-V2.0 header T5 (serial console, pin 1 square):
#
#   Pico GP0 (pin 1, UART0 TX)  ->  T5 pin 3  (U0RXD, the switch's input)
#   Pico GP1 (pin 2, UART0 RX)  ->  T5 pin 1  (U0TXD, the switch's output)
#   Pico GND (pin 3)            ->  T5 pin 2  (GND)
#
# Leave T5 pin 4 (3.3 V) unconnected and power the Pico from USB.

from machine import UART, Pin
import time
import bridge_link as bl

BAUD = 976562           # LINK_TH1 0xfe in modchip/bridge/bridge.h
PORTS = (("RJ45 1", 4), ("RJ45 2", 5), ("RJ45 3", 6), ("RJ45 4", 7),
         ("SFP J4", 8), ("SFP J2", 3))

uart = UART(0, baudrate=BAUD, tx=Pin(0), rx=Pin(1), rxbuf=4096, timeout=0)
deframer = bl.Deframer()
seq = 0
stats = {"rx_frames": 0, "logs": 0}


def show(msg):
    t, s, p = msg
    if t == bl.MSG_LOG:
        stats["logs"] += 1
        print("[8051]", bytes(p).decode("utf-8", "replace").rstrip())
    elif t == bl.MSG_BEACON:
        h = bl.parse_hello(p)
        print("beacon:", h["machine"], h["version"]) if h else print("beacon (short)")
    elif t == bl.MSG_LINK:
        sts = bl.u32(p, 0)
        print("links:", ", ".join("%s %s" % (n, "up" if sts >> (16 + port if port < 8 else port) & 1
                                              else "down") for n, port in PORTS))
    elif t == bl.MSG_SFP:
        print("sfp pins: 0x%02x" % p[0])
    elif t == bl.MSG_RX_FRAME:
        stats["rx_frames"] += 1
        frame = p[8:]
        print("rx frame: port %d, %d bytes, dst %s src %s" % (
            p[3] & 0xF, bl.u16(p, 4), bytes(frame[0:6]).hex(), bytes(frame[6:12]).hex()))


def poll(ms):
    """Show unsolicited messages for ms milliseconds"""
    end = time.ticks_add(time.ticks_ms(), ms)
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        data = uart.read()
        if data:
            for m in deframer.feed(data):
                show(m)
        else:
            time.sleep_ms(1)


def request(req, payload=b"", timeout_ms=1000):
    """Send a request, show what arrives meanwhile, return the reply payload"""
    global seq
    seq = (seq + 1) & 0xFF
    uart.write(bl.encode(req, seq, payload))
    end = time.ticks_add(time.ticks_ms(), timeout_ms)
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        data = uart.read()
        if not data:
            time.sleep_ms(1)
            continue
        for t, s, p in deframer.feed(data):
            if t == req | bl.REPLY and s == seq:
                if p[0] != 0:
                    raise RuntimeError("request 0x%02x: %s" % (req, bl.STATUS.get(p[0], p[0])))
                return p[1:]
            show((t, s, p))
    raise RuntimeError("request 0x%02x: no reply" % req)


def main():
    print("Listening for the bridge at %d baud..." % BAUD)
    poll(2500)
    if deframer.errors:
        print("framing errors so far:", deframer.errors)

    h = bl.parse_hello(request(bl.REQ_HELLO))
    for k in ("machine", "version", "mac", "chip_id", "flash_size", "image_size", "upload_start"):
        v = h[k]
        print("%-13s %s" % (k, "0x%x" % v if isinstance(v, int) else v))
    poll(200)       # the link and SFP state follow the hello

    p = request(bl.REQ_PING)
    print("uptime %d s, rx dropped %d, link errors %d" % (bl.u32(p), bl.u16(p, 4), bl.u16(p, 6)))

    print("chip id via the bridge: 0x%08x" % bl.u32(request(bl.REQ_REG_READ, bl.p16(0x0004))))

    # PHY of RJ45 port 1 (logical port 4): MMD 1 (PMA/PMD), device ID registers
    try:
        id1 = bl.u16(request(bl.REQ_PHY_READ, bytes((4, 1)) + bl.p16(2)))
        id2 = bl.u16(request(bl.REQ_PHY_READ, bytes((4, 1)) + bl.p16(3)))
        print("port 4 PHY ID: 0x%04x%04x" % (id1, id2))
    except RuntimeError as e:
        print(e)

    for slot in range(h["n_sfp"]):
        try:
            ident = request(bl.REQ_SFP_READ, bytes((slot, 0, 1)))
            print("SFP slot %d identifier byte: 0x%02x" % (slot, ident[0]))
        except RuntimeError as e:
            print("SFP slot %d: %s (empty?)" % (slot, e))

    print("Monitoring for 30 s; plug and unplug cables to see link events")
    for _ in range(30):
        request(bl.REQ_PING)    # keeps the bridge forwarding to us
        poll(1000)
    print("frames received: %d, log lines: %d, framing errors: %d" % (
        stats["rx_frames"], stats["logs"], deframer.errors))


main()
