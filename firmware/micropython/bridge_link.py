# Host side of the mod-chip bridge protocol (bridge_proto.h in the bridge firmware:
# https://github.com/wrcrooks/RTLPlayground/tree/feat/modchip/modchip/bridge).
#
# Plain Python that runs both under MicroPython on the RP2350 and under
# CPython, where test_bridge_link.py exercises it.

PROTO_VERSION = 1

REQ_HELLO = 0x01
REQ_PING = 0x02
REQ_TX_FRAME = 0x10
REQ_REG_READ = 0x20
REQ_REG_WRITE = 0x21
REQ_PHY_READ = 0x22
REQ_PHY_WRITE = 0x23
REQ_SDS_READ = 0x24
REQ_SDS_WRITE = 0x25
REQ_SFP_READ = 0x26
REQ_TBL_READ = 0x27
REQ_SDS_CONFIG = 0x28
REQ_STAT_READ = 0x29
REQ_FLASH_READ = 0x30
REQ_FLASH_ERASE = 0x31
REQ_FLASH_WRITE = 0x32
REQ_FLASH_ID = 0x33
REQ_SET_LED = 0x3E
REQ_REBOOT = 0x3F
REPLY = 0x80

MSG_BEACON = 0x40
MSG_RX_FRAME = 0x41
MSG_LINK = 0x42
MSG_SFP = 0x43
MSG_LOG = 0x44

STATUS = {0: "ok", 1: "bad length", 2: "bad argument", 3: "failed", 4: "unknown request"}


def crc16(data, crc=0xFFFF):
    """CRC-16/MODBUS, the table the 8051 uses in crc16.asm"""
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def cobs_encode(data):
    out = bytearray()
    block = bytearray()
    for b in data:
        if b == 0:
            out.append(len(block) + 1)
            out += block
            block = bytearray()
        else:
            block.append(b)
            if len(block) == 254:
                out.append(0xFF)
                out += block
                block = bytearray()
    out.append(len(block) + 1)
    out += block
    return bytes(out)


def cobs_decode(data):
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        code = data[i]
        if code == 0 or i + code > n:
            raise ValueError("bad COBS code")
        block = data[i + 1:i + code]
        if 0 in block:
            raise ValueError("bad COBS block")
        out += block
        i += code
        if code != 0xFF and i < n:
            out.append(0)
    return bytes(out)


def encode(msg_type, seq, payload=b""):
    body = bytes((msg_type, seq & 0xFF)) + bytes(payload)
    crc = crc16(body)
    return cobs_encode(body + bytes((crc & 0xFF, crc >> 8))) + b"\x00"


def decode(frame):
    """One frame without its 0x00 delimiter -> (type, seq, payload)"""
    body = cobs_decode(frame)
    if len(body) < 4 or crc16(body) != 0:
        raise ValueError("bad CRC or short frame")
    return body[0], body[1], body[2:-2]


def u16(b, o=0):
    return b[o] | b[o + 1] << 8


def u32(b, o=0):
    return u16(b, o) | u16(b, o + 2) << 16


def p16(v):
    return bytes((v & 0xFF, v >> 8 & 0xFF))


def p32(v):
    return p16(v & 0xFFFF) + p16(v >> 16 & 0xFFFF)


def parse_hello(p):
    name = bytes(p[28:58])
    return {
        "proto": p[0],
        "chip_id": u32(p, 1),
        "mac": ":".join("%02x" % b for b in p[5:11]),
        "flash_size": u32(p, 11),
        "image_size": u32(p, 15),
        "upload_start": u32(p, 19),
        "cpu_port": p[23],
        "min_port": p[24],
        "max_port": p[25],
        "n_sfp": p[26],
        "flags": p[27],
        "machine": name.split(b"\x00")[0].decode(),
        "version": bytes(p[58:]).decode(),
    } if len(p) >= 58 else None


class Deframer:
    """Feeds raw UART bytes, returns complete decoded messages"""

    def __init__(self):
        self.buf = bytearray()
        self.errors = 0

    def feed(self, data):
        msgs = []
        for b in data:
            if b:
                if len(self.buf) < 2048:
                    self.buf.append(b)
                continue
            if self.buf:
                try:
                    msgs.append(decode(bytes(self.buf)))
                except ValueError:
                    self.errors += 1
            self.buf = bytearray()
        return msgs
