#!/usr/bin/env python3
"""Signed firmware updates for the mod chip, and the image that makes a board
take them. Needs the openssl command.

  sign_update.py keygen <key.pem>
      Makes a signing key. Keep it to yourself, and out of git: whoever has
      it can update your boards. Build with -DMODCHIP_UPDATE_KEY=<key.pem>.
  sign_update.py sign <key.pem> <modchip.bin> <out.mcu> [--seq N]
      An update file for POST /update or the web UI. The sequence number (by
      default the time) must not go down from one update to the next.
  sign_update.py factory <modchip.bin> <out.bin> [--seq N] [--flash-size BYTES]
      The partition table and the firmware in slot A, to be flashed over SWD
      at 0x10000000, once. The settings at the flash's end stay.
  sign_update.py header <key.pem or ""> <out.h>
      The public key for the build (CMakeLists.txt runs it).
  sign_update.py info <file.mcu>
"""

import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import time

HDR = struct.Struct("<8s48s32sII32s")
SECTOR = 4096
SLOT_A = 2 * SECTOR		# the partition table comes first
DATA = 16 * SECTOR		# settings at the end, main.c, tls.c
SLOT_MAX = 0x180000		# a slot needs no more on a larger flash

START, END = 0xFFFFDED3, 0xAB123579
IMAGE_TYPE, VERSION, HASH_DEF, HASH_VALUE, SIGNATURE, PARTITION_TABLE, LAST = 0x42, 0x48, 0x47, 0x4B, 0x09, 0x0A, 0xFF
TBYB = 0x8000


def die(text):
    sys.exit("sign_update.py: " + text)


def openssl(*args, data=None):
    r = subprocess.run(["openssl"] + list(args), input=data, capture_output=True)
    if r.returncode:
        die("openssl %s: %s" % (args[0], r.stderr.decode().strip()))
    return r.stdout


def public_point(key):
    der = openssl("pkey", "-in", key, "-pubout", "-outform", "DER")
    if len(der) != 91 or der[26] != 4:
        die("%s is not a P-256 key" % key)
    return der[26:]


def raw_signature(der):
    """ECDSA's SEQUENCE of two INTEGERs as r and s, 32 bytes each"""
    def integer(p):
        if der[p] != 2:
            die("odd signature")
        n = der[p + 1]
        return int.from_bytes(der[p + 2:p + 2 + n], "big"), p + 2 + n
    if der[0] != 0x30:
        die("odd signature")
    r, p = integer(2)
    s, p = integer(p)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def mark(image, name):
    i = image.find(name.encode())
    if i < 0:
        die("no %s in the image: is it the mod chip's firmware?" % name)
    i += len(name)
    return image[i:image.index(b"\0", i)]


def patch(image, seq, trial):
    """The boot ROM's marks in the IMAGE_DEF: the version, try before you buy"""
    image = bytearray(image)
    at = next((i for i in range(0, 4096 - 4, 4) if struct.unpack_from("<I", image, i)[0] == START), None)
    if at is None:
        die("no IMAGE_DEF in the first 4 KB")
    p, seen = at + 4, set()
    while True:
        kind, size = image[p], image[p + 1]
        if kind == LAST:
            break
        if kind & 0x80:
            size = struct.unpack_from("<H", image, p + 1)[0]
        if kind == IMAGE_TYPE:
            flags = struct.unpack_from("<H", image, p + 2)[0]
            struct.pack_into("<H", image, p + 2, flags | TBYB if trial else flags & ~TBYB)
        elif kind == VERSION:
            if size != 2:
                die("a rollback version in the IMAGE_DEF")
            struct.pack_into("<I", image, p + 4, seq)
        elif kind in (HASH_DEF, HASH_VALUE, SIGNATURE):
            die("the image is sealed, its IMAGE_DEF can't be changed")
        seen.add(kind)
        if not size:
            die("malformed IMAGE_DEF")
        p += 4 * size
    if not {IMAGE_TYPE, VERSION} <= seen:
        die("no version in the IMAGE_DEF")
    return bytes(image)


def slots(flash_size):
    size = min((flash_size - DATA - SLOT_A) // 2 // SECTOR * SECTOR, SLOT_MAX)
    return SLOT_A, SLOT_A + size, size


def partition_table(flash_size):
    a, b, size = slots(flash_size)
    rw, arm = 0x3F << 26, 1 << 17
    words = [0x3F << 26 | 1 << 15]	# the rest: anybody, absolute addresses
    for start, flags in ((a, arm), (b, arm | 1 << 1)):	# B is A's other half
        words += [rw | start // SECTOR | ((start + size) // SECTOR - 1) << 13, rw | flags]
    words = [PARTITION_TABLE | (len(words) + 1) << 8 | 2 << 24] + words
    return struct.pack("<%dI" % (len(words) + 4), START, *words, LAST | len(words) << 8, 0, END)


def seq_of(args):
    seq = int(time.time()) if args.seq is None else args.seq
    if not 0x10000 <= seq < 1 << 32:
        die("the sequence number must be from 65536 on and 32 bits")
    return seq


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="do", required=True)
    for name, files in (("keygen", ["key"]), ("sign", ["key", "image", "out"]), ("factory", ["image", "out"]),
                        ("header", ["key", "out"]), ("info", ["file"])):
        p = sub.add_parser(name)
        for f in files:
            p.add_argument(f)
        if name in ("sign", "factory"):
            p.add_argument("--seq", type=int)
            p.add_argument("--flash-size", type=lambda x: int(x, 0), default=2 * 1024 * 1024)
    args = ap.parse_args()

    if args.do == "keygen":
        if os.path.exists(args.key):
            die("%s exists" % args.key)
        pem = openssl("genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256")
        with os.fdopen(os.open(args.key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "wb") as f:
            f.write(pem)
        print("%s: keep it secret, keep a copy" % args.key)
        return
    if args.do == "header":
        text = "/* Made by tools/sign_update.py */\n"
        if args.key:
            text += "#define UPDATE_KEY " + ", ".join("0x%02x" % c for c in public_point(args.key)) + "\n"
        if not os.path.exists(args.out) or open(args.out).read() != text:
            with open(args.out, "w") as f:
                f.write(text)
        return
    if args.do == "info":
        d = open(args.file, "rb").read()
        magic, board, version, seq, n, h = HDR.unpack(d[:128])
        if magic != b"MCUPD1\0\0":
            die("not an update file")
        print("board %s\nversion %s\nsequence %d\nimage %d bytes, SHA-256 %s" % (
            board.rstrip(b"\0").decode(), version.rstrip(b"\0").decode(), seq, n, h.hex()))
        print("the image is %s" % ("intact" if hashlib.sha256(d[192:]).digest() == h and len(d) == 192 + n else "DAMAGED"))
        return

    image = open(args.image, "rb").read()
    seq = seq_of(args)
    _, _, size = slots(args.flash_size)
    board, version = mark(image, "MCBOARD:"), mark(image, "MCVERSION:")
    if len(image) > size:
        die("%d bytes don't fit a slot of %d" % (len(image), size))
    if len(board) > 47 or len(version) > 31:
        die("board or version too long")
    if args.do == "factory":
        out = partition_table(args.flash_size).ljust(SLOT_A, b"\xff") + patch(image, seq, False)
        with open(args.out, "wb") as f:
            f.write(out)
        print("%s: %s %s, number %d; slots of %d KB" % (args.out, board.decode(), version.decode(), seq, size // 1024))
        return
    image = patch(image, seq, True)
    hdr = HDR.pack(b"MCUPD1\0\0", board, version, seq, len(image), hashlib.sha256(image).digest())
    with tempfile.NamedTemporaryFile() as t:
        t.write(hdr)
        t.flush()
        sig = raw_signature(openssl("dgst", "-sha256", "-sign", args.key, t.name))
    with open(args.out, "wb") as f:
        f.write(hdr + sig + image)
    print("%s: %s %s, number %d" % (args.out, board.decode(), version.decode(), seq))


if __name__ == "__main__":
    main()
