#!/usr/bin/env python3
"""Update files from tools/sign_update.py through update.c: run_update.py <test_update> <key> <another key>"""

import os
import random
import struct
import subprocess
import sys
import tempfile

BIN, KEY, KEY2 = sys.argv[1:4]
TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../tools/sign_update.py")
failures = 0


def check(ok, what):
    global failures
    print("%s  %s" % ("PASS" if ok else "FAIL", what))
    failures += not ok


def image(n, board=b"modchip TEST"):
    """As the linker makes it: an IMAGE_DEF with a version somewhere at the start"""
    rnd = random.Random(n)
    b = bytearray(rnd.randrange(256) for _ in range(n))
    block = struct.pack("<IBBHBBHIBHBII", 0xFFFFDED3, 0x42, 1, 0x1021, 0x48, 2, 0, 0x10000, 0xFF, 3, 0, 0, 0xAB123579)
    b[0x138:0x138 + len(block)] = block
    b[0x400:0x440] = (b"MCBOARD:" + board + b"\0MCVERSION:v9.9-test\0").ljust(64, b"\0")
    return bytes(b)


def run(*a):
    return subprocess.run([BIN] + list(a), capture_output=True, text=True, timeout=60).stdout


def tool(*a):
    return subprocess.run([sys.executable, TOOL] + list(a), capture_output=True, text=True)


def main():
    out = subprocess.run([BIN, "life"], capture_output=True, text=True)
    print(out.stdout, end="")
    check(out.returncode == 0, "an update's life")
    with tempfile.TemporaryDirectory() as d:
        p = lambda n: os.path.join(d, n)

        def make(name, img, key=KEY, seq="200000"):
            open(p("img"), "wb").write(img)
            r = tool("sign", key, p("img"), p(name), "--seq", seq)
            check(r.returncode == 0, "signed %s" % name) if r.returncode else None
            return p(name)

        good = make("good", image(50001))
        check("intact" in tool("info", good).stdout, "a file checks out")
        data = open(good, "rb").read()
        check(data[192 + 0x138 + 6:192 + 0x138 + 8] == b"\x21\x90" and struct.unpack_from("<I", data, 192 + 0x138 + 12)[0] == 200000,
              "the IMAGE_DEF: to be tried before it is bought, the number its version")
        out = run(good)
        check(out.startswith("OK\nslot good") and "EARLY" not in out, "taken, the first sector last: %r" % out)
        for n in (4096, 4097, 8192, 65536):
            check(run(make("n", image(n))).startswith("OK\nslot good"), "%d bytes" % n)
        check(run(good, "seq=200000").startswith("OK"), "the same number again")
        check("older" in run(good, "seq=200001") and "untouched" in run(good, "seq=200001"), "an older one: refused, nothing written")
        check("signature" in run(make("k2", image(50001), KEY2)) and "untouched" in run(p("k2")), "another key")
        check("is for modchip OTHER" in run(make("b", image(50001, b"modchip OTHER"))), "another board")
        r = run(make("big", image(65537)))
        check("fit" in r, "too big for the slot: %r" % r)
        check("no A/B" in run(good, "noslots"), "no slots")
        check("on trial" in run(good, "trial"), "not while one is on trial")
        for off, what in ((3, "magic"), (9, "board"), (88, "number"), (92, "length"), (100, "hash"), (130, "r"), (170, "s")):
            b = bytearray(data)
            b[off] ^= 1
            open(p("x"), "wb").write(b)
            r = run(p("x"))
            check(r.startswith("NO:") and "untouched" in r, "a changed %s: refused, nothing written" % what)
        for sig, what in ((bytes(64), "zeros"), (b"\xff" * 64, "ones"), (bytes(31) + b"\x01" + bytes(31) + b"\x01", "1, 1")):
            open(p("x"), "wb").write(data[:128] + sig + data[192:])
            check("signature" in run(p("x")), "a signature of " + what)
        for off in (192, 192 + 5000, len(data) - 1):
            b = bytearray(data)
            b[off] ^= 1
            open(p("x"), "wb").write(b)
            r = run(p("x"))
            check("damaged" in r and "blank" in r, "a changed byte in the image at %d: the slot stays blank" % off)
        open(p("x"), "wb").write(data[:-1])
        check("not as long" in run(p("x")), "a byte short")
        open(p("x"), "wb").write(data + b"\0")
        check("not as long" in run(p("x")), "a byte long")
        open(p("x"), "wb").write(data[:100])
        check("too short" in run(p("x")), "just a header")
        for stop in (10, 192, 3000, 6000, 40000):
            r = run(good, "stop=%d" % stop)
            check("slot good" not in r and ("untouched" in r or "blank" in r), "stopped after %d: %s" % (stop, r.split("\n")[1]))
        check("damaged" in run(good, "break=8192") and "blank" in run(good, "break=8192"), "flash that doesn't hold: the slot stays blank")
        check("reads back wrong" in run(good, "break=0") and "blank" in run(good, "break=0"), "  nor in the first sector")

        open(p("img"), "wb").write(image(50000))
        r = tool("factory", p("img"), p("f"), "--seq", "200000")
        f = open(p("f"), "rb").read()
        w = struct.unpack("<10I", f[:40])
        check(r.returncode == 0 and w == (0xFFFFDED3, 0x0200060A, 0xFC008000, 0xFC000000 | 2 | 0xF8 << 13, 0xFC020000,
              0xFC000000 | 0xF9 | 0x1EF << 13, 0xFC020002, 0x6FF, 0, 0xAB123579), "factory: the partition table (as picotool reads it)")
        check(f[8192 + 0x138 + 6:8192 + 0x138 + 8] == b"\x21\x10", "  the image in slot A, already bought")
        open(p("img"), "wb").write(image(50000).replace(b"MCBOARD", b"MCBORED"))
        check(tool("sign", KEY, p("img"), p("f")).returncode != 0, "not the firmware: not signed")
        open(p("img"), "wb").write(image(1100000))
        check(tool("factory", p("img"), p("f")).returncode != 0, "too big for the flash: no factory image")
        r = tool("keygen", KEY)
        check(r.returncode != 0 and "exists" in r.stderr, "keygen overwrites nothing")
    print("FAILED" if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
