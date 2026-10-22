#!/usr/bin/env python3
"""bridge_link.py against the 8051 bridge firmware's own framing.

bridge_vectors.txt holds the frames the bridge's link.c produces for ten
payloads that stress COBS (empty, zero runs, 254-byte blocks, a full-size
frame). It comes from RTLPlayground's test binary, which compiles link.c
unchanged:

  make -C test build/test_bridge_link && test/build/test_bridge_link --emit

(wrcrooks/RTLPlayground, branch feat/modchip). Regenerate it there whenever
the framing changes, and copy it here.

  python3 test_bridge_link.py
"""
import pathlib, sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bridge_link as bl  # noqa: E402

failures = 0


def check(cond, name):
    global failures
    print(("PASS  " if cond else "FAIL  ") + name)
    failures += not cond


def vector(k):
    """As vector() in RTLPlayground's test/test_bridge_link.c"""
    lens = (0, 1, 1, 253, 254, 255, 508, 509, 300, 1557)  # 1557: the largest payload, LINK_MSG_MAX - 4
    x = 12345 + k
    out = bytearray()
    for _ in range(lens[k]):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        if k in (1, 8):
            out.append(0)
        elif 3 <= k <= 7:
            out.append(1 + (x >> 16) % 255)
        else:
            out.append((x >> 16) & 0xFF)
    return bytes(out)


check(bl.crc16(b"123456789") == 0x4B37, "CRC-16/MODBUS check value")

frames = (HERE / "bridge_vectors.txt").read_text().split()
check(len(frames) == 10, "ten vectors from the bridge's link.c")
for k, line in enumerate(frames):
    frame = bytes.fromhex(line)
    check(frame == bl.encode(0x10 + k, k, vector(k)), "vector %d: encodes as the bridge does" % k)
    check(bl.Deframer().feed(frame) == [(0x10 + k, k, vector(k))], "vector %d: decodes the bridge's frame" % k)
    bad = bytearray(frame)
    bad[1] ^= 0x01 if bad[1] != 0x01 else 0x03
    check(bl.Deframer().feed(bytes(bad)) == [], "vector %d: refuses a corrupted frame" % k)

# The hello payload layout as put_hello() in the bridge writes it
hello = (bytes((1,)) + bl.p32(0x83720000) + bytes.fromhex("060f00dd3b3e") + bl.p32(0x80000)
         + bl.p32(0x3F000) + bl.p32(0x3F000) + bytes((9, 3, 8, 2, 4))
         + b"SWGT024 V2.0 Unmanaged".ljust(30, b"\x00") + b"v0.1.0-abc1234")
h = bl.parse_hello(hello)
check(h["chip_id"] == 0x83720000 and h["mac"] == "06:0f:00:dd:3b:3e" and h["cpu_port"] == 9
      and h["max_port"] == 8 and h["machine"] == "SWGT024 V2.0 Unmanaged"
      and h["version"] == "v0.1.0-abc1234", "hello payload parses")

sys.exit(1 if failures else 0)
