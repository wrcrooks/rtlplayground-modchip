#!/usr/bin/env python3
"""Seeds for fuzz_unifi from the captured configurations: fuzz_seeds.py <dir>"""
import json, os, sys, zlib

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
here = os.path.dirname(os.path.abspath(__file__))
cfgs = [open(os.path.join(here, "unifi", f), "rb").read() for f in ("adopted.cfg", "ports.cfg")]
seeds = []
for c in cfgs:
    reply = json.dumps({"_type": "setparam", "cfgversion": "0123456789abcdef", "interval": 10,
                        "mgmt_cfg": "authkey=00112233445566778899aabbccddeeff\nuse_aes_gcm=true\ncfgversion=1\n",
                        "system_cfg": c.decode()}).encode()
    seeds += [b"\x01\x05" + zlib.compress(reply), b"\x01\x03" + zlib.compress(reply), b"\x01\x04" + reply,
              b"\x02\xff" + zlib.compress(c, 9), b"\x02\x10" + zlib.compress(c, 1), b"\x03" + reply,
              b"\x04\x04\x02\xff" + cfgs[0] + b"\0" + c]
seeds.append(b"\x03" + json.dumps({"_type": "cmd", "cmd": "set-adopt", "key": "00" * 16,
                                   "uri": "http://10.0.0.1:8080/inform"}).encode())
for i, s in enumerate(seeds):
    open(os.path.join(out, "seed%02d" % i), "wb").write(s)
