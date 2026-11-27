#!/usr/bin/env python3
"""Vectors for test_unifi (see there), from Python's zlib and cryptography."""
import os, random, struct, subprocess, sys, zlib
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives import padding

def seal(mac, data, key, gcm, flags_extra=0):
    iv = os.urandom(16); data = zlib.compress(data)
    if gcm:
        hdr = lambda n: b"TNBU" + struct.pack(">I", 1) + mac + struct.pack(">H", 0x0b) + iv + struct.pack(">II", 1, n)
        return hdr(len(data) + 16) + AESGCM(key).encrypt(iv, data, hdr(len(data) + 16))
    p = padding.PKCS7(128).padder(); data = p.update(data) + p.finalize()
    e = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor(); body = e.update(data) + e.finalize()
    return b"TNBU" + struct.pack(">I", 1) + mac + struct.pack(">H", 0x03) + iv + struct.pack(">II", 1, len(body)) + body

def unseal(pkt, key):
    flags, = struct.unpack(">H", pkt[14:16]); iv = pkt[16:32]; n, = struct.unpack(">I", pkt[36:40]); body = pkt[40:40 + n]
    assert len(body) == n and pkt[:4] == b"TNBU"
    if flags & 8: data = AESGCM(key).decrypt(iv, body, pkt[:40])
    else:
        d = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor(); data = d.update(body) + d.finalize()
        u = padding.PKCS7(128).unpadder(); data = u.update(data) + u.finalize()
    assert flags & 2
    return zlib.decompress(data)

def main():
    exe, cfgdir = sys.argv[1], sys.argv[2]
    rnd = random.Random(7); lines = []; seals = []
    texts = [b"", b"a", b'{"_type":"noop","interval":10}', open(os.path.join(cfgdir, "ports.cfg"), "rb").read(),
             bytes(rnd.randrange(256) for _ in range(3000)), b"abc" * 20000,
             bytes(rnd.choice(b"abcd \n=") for _ in range(40000))]
    for t in texts:
        for level in (0, 1, 6, 9):
            lines.append("Z %s %s" % (zlib.compress(t, level).hex() or "", t.hex() or "E"))
        for strategy in (zlib.Z_FIXED, zlib.Z_HUFFMAN_ONLY, zlib.Z_RLE):
            c = zlib.compressobj(9, zlib.DEFLATED, 15, 9, strategy); z = c.compress(t) + c.flush()
            lines.append("Z %s %s" % (z.hex(), t.hex() or "E"))
    good = zlib.compress(texts[3])
    for i in range(200):
        bad = bytearray(good); bad[rnd.randrange(2, len(bad))] ^= 1 << rnd.randrange(8)
        lines.append("X %s" % bytes(bad).hex())
    lines.append("X %s" % good[:-1].hex())
    lines.append("X %s" % good[:len(good) // 2].hex())
    mac = bytes.fromhex("02005e100001")
    for t in texts[1:5]:
        for gcm in (False, True):
            key = os.urandom(16)
            lines.append("P %s %s %s" % (key.hex(), seal(mac, t, key, gcm).hex(), t.hex()))
            key2 = os.urandom(16); seals.append((key2, t))
            lines.append("S %s %d %s" % (key2.hex(), gcm, t.hex()))
    r = subprocess.run([exe, os.path.join(cfgdir, "adopted.cfg"), os.path.join(cfgdir, "ports.cfg"),
                        os.path.join(cfgdir, "features.cfg")],
                       input="\n".join(lines) + "\n", capture_output=True, text=True)
    out = r.stdout.splitlines()
    for l in out:
        if l.startswith("FAIL"): print(l)
    got = [bytes.fromhex(l[2:]) for l in out if l.startswith("S ")]
    ok = len(got) == len(seals)
    for pkt, (key, t) in zip(got, seals):
        if unseal(pkt, key) != t: ok = False; print("FAIL python can't open a sealed packet")
    cmds = {}; cur = None
    for l in out:
        if l.startswith("C "): cur = l[2:]; cmds[cur] = []
        elif cur and cur != "end": cmds[cur].append(l)
    want = {
        "adopted": ["vlan 1 1 2 3 4 5 6", "vlan 10 1t 2t 3t 4t 5t 6t", "vlan 20 1t 2t 3t 4t 5t 6t",
                    "pvid 1 1", "pvid 2 1", "pvid 3 1", "pvid 4 1", "pvid 5 1", "pvid 6 1",
                    "stp version rstp", "stp prio 8", "stp on", "stp port 1 on", "stp port 1 guard none",
                    "stp port 2 on", "stp port 2 guard none", "stp port 3 on", "stp port 3 guard none",
                    "stp port 4 on", "stp port 4 guard none"],
        "ports": ["vlan 1 1 3 4 5 6", "vlan 10 1t 2 3t 4t 5t 6t", "vlan 20 1t 2t 3t 4t 5t 6t",
                  "pvid 1 1", "pvid 2 10", "port 2 name Camera", "pvid 3 1", "pvid 4 1", "port 4 100m full",
                  "pvid 5 1", "pvid 6 1"],
        "back": ["vlan 1 1 2 3 4 5 6", "vlan 10 1t 2t 3t 4t 5t 6t", "vlan 20 1t 2t 3t 4t 5t 6t",
                 "pvid 1 1", "pvid 2 1", "port 2 name", "pvid 3 1", "pvid 4 1", "port 4 auto", "pvid 5 1", "pvid 6 1"],
        # SFP+ UniFi port 9 is front port 5; port 3's storm limit doesn't count, its storm control is off
        "features": ["vlan 1 1 2 3 4 5 6", "vlan 10 1t 2t 3t 4t 5t 6t", "vlan 20 1t 2t 3t 4t 5t 6t",
                     "pvid 1 1", "pvid 2 1", "pvid 3 1", "pvid 4 1", "pvid 5 1", "pvid 6 1",
                     "stp prio 1", "stp port 1 edge on", "stp port 1 guard bpdu", "stp port 4 off",
                     "isolate 1 3 4 5 6 0", "isolate 2 3 4 5 6 0", "isolate 3 off", "isolate 4 off",
                     "isolate 5 off", "isolate 6 off",
                     "storm 2 bcast 100 pps", "storm 2 mcast 200 pps", "storm 2 ucast 300 pps",
                     "bw out 3 00001388", "bw out 5 000186a0"],
        "unfeatures": ["vlan 1 1 2 3 4 5 6", "vlan 10 1t 2t 3t 4t 5t 6t", "vlan 20 1t 2t 3t 4t 5t 6t",
                       "pvid 1 1", "pvid 2 1", "pvid 3 1", "pvid 4 1", "pvid 5 1", "pvid 6 1",
                       "stp prio 8", "stp port 1 guard none", "stp port 4 on", "stp port 4 guard none",
                       "isolate 1 off", "isolate 2 off", "isolate 3 off", "isolate 4 off", "isolate 5 off",
                       "isolate 6 off", "storm 2 bcast off", "storm 2 mcast off", "storm 2 ucast off",
                       "bw out 3 off", "bw out 5 off"],
    }
    for k, v in want.items():
        if cmds.get(k) != v: ok = False; print("FAIL commands for %s:\n  got  %s\n  want %s" % (k, cmds.get(k), v))
    print(out[-1] if out else r.stderr)
    sys.exit(0 if ok and r.returncode == 0 else 1)

main()
