#!/usr/bin/env python3
"""telemetry.c against a small MQTT broker, in plain and over TLS: run_mqtt.py <test_mqtt>"""

import hashlib
import json
import os
import queue
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time

failures = 0


def check(ok, what):
    global failures
    print("%s  %s" % ("PASS" if ok else "FAIL", what))
    failures += not ok


class Broker:
    """Takes connections on a port of its own; what arrives goes to self.q"""

    def __init__(self, ctx=None, refuse=0):
        self.s = socket.create_server(("127.0.0.1", 0))
        self.port = self.s.getsockname()[1]
        self.ctx, self.refuse, self.q, self.conn = ctx, refuse, queue.Queue(), None
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            c, _ = self.s.accept()
            try:
                if self.ctx:
                    c = self.ctx.wrap_socket(c, server_side=True)
                    self.q.put(("tls", c.version()))
                self.conn = c
                self.client(c)
            except (OSError, EOFError) as e:
                self.q.put(("gone", str(e)))

    def client(self, c):
        f = c.makefile("rb")
        while True:
            h = f.read(1)
            if not h:
                self.q.put(("gone", "closed"))
                return
            n, shift = 0, 0
            while True:
                b = f.read(1)[0]
                n |= (b & 127) << shift
                shift += 7
                if not b & 128:
                    break
            d = f.read(n)
            kind = h[0] >> 4
            if kind == 1:
                parts, p = [], d[10:]
                while p:
                    k = struct.unpack(">H", p[:2])[0]
                    parts.append(p[2:2 + k])
                    p = p[2 + k:]
                self.q.put(("connect", d[7], struct.unpack(">H", d[8:10])[0], parts))
                c.sendall(bytes([0x20, 2, 0, self.refuse]))
            elif kind == 3:
                k = struct.unpack(">H", d[:2])[0]
                self.q.put(("publish", d[2:2 + k].decode(), d[2 + k:].decode(), bool(h[0] & 1)))
            elif kind == 12:
                c.sendall(b"\xd0\0")
            elif kind == 14:
                self.q.put(("disconnect",))

    def get(self, t=8, want=None):
        end = time.time() + t
        while True:
            try:
                m = self.q.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                return None
            if want is None or m[0] == want:
                return m


def start(broker, *a):
    return subprocess.Popen([sys.argv[1], str(broker.port)] + list(a), stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE)


def a_round(b, prefix):
    got = {}
    while len(got) < 12:
        m = b.get(10, "publish")
        if not m:
            break
        if not m[1].endswith("/event"):
            got[m[1]] = m
    ok = got.get(prefix + "/status", (0, 0, 0, 0))[2:] == ("online", True)
    for i in range(1, 11):
        m = got.get("%s/port/%d" % (prefix, i))
        ok = ok and bool(m) and json.loads(m[2])["rx_bytes"] == i and m[3]
    return ok and json.loads(got[prefix + "/switch"][2])["temp"] == 45.5


def main():
    b = Broker()
    p = start(b, "login=me,se cret")
    m = b.get()
    check(m and m[0] == "connect" and m[3] == [b"modchip-060f00dd3b3e", b"testswitch/status", b"offline", b"me", b"se cret"]
          and m[1] & 0xe4 == 0xe4 and m[2] == 30, "connects, with a last will and a login: %s" % (m,))
    check(a_round(b, "testswitch"), "online, the switch and ten ports, retained")
    p.stdin.write(b'something "odd"\n')
    p.stdin.flush()
    m = None
    while not m or m[1] != "testswitch/event" or "odd" not in m[2]:
        m = b.get(5, "publish")
        if not m:
            break
    check(m and json.loads(m[2])["text"] == 'something "odd"' and json.loads(m[2])["port"] == 2 and not m[3], "an event")
    for i in range(20):
        p.stdin.write(b"burst %d\n" % i)
    p.stdin.flush()
    t, got = time.time(), []
    while time.time() < t + 4:
        m = b.get(1, "publish")
        if m and m[1] == "testswitch/event":
            got.append(json.loads(m[2])["text"])
    check(got == ["burst %d" % i for i in range(12, 20)], "more events than fit: the latest eight")
    b.conn.shutdown(socket.SHUT_RDWR)
    b.conn.close()
    check(b.get(30, "connect") is not None, "connects again when the connection is lost")
    check(a_round(b, "testswitch"), "and starts over")
    p.stdin.write(b"!topic lab/sw1\n")
    p.stdin.flush()
    m = b.get(20, "connect")
    check(m and m[3][1] == b"lab/sw1/status", "another topic, another connection")
    check(a_round(b, "lab/sw1"), "everything below the new topic")
    p.stdin.write(b"!off\n")
    p.stdin.flush()
    while m and m[0] not in ("disconnect", "gone"):
        last, m = m, b.get(5)
    check(last[:3] == ("publish", "lab/sw1/status", "offline"), "off: says so first")
    p.kill()

    b = Broker(refuse=5)
    p = start(b)
    b.get(5, "connect")
    check(b.get(5, "gone") is not None and b.get(10, "connect") is None and b.get(10, "connect") is not None,
          "refused: tries again, not too soon")
    p.kill()

    with tempfile.TemporaryDirectory() as d:
        for kind, args in (("ECDSA P-256", ["ec", "-pkeyopt", "ec_paramgen_curve:P-256"]), ("ECDSA P-384", ["ec", "-pkeyopt", "ec_paramgen_curve:P-384"]),
                           ("RSA 2048", ["rsa:2048"]), ("RSA 4096", ["rsa:4096"])):
            crt, key = os.path.join(d, "c"), os.path.join(d, "k")
            subprocess.run(["openssl", "req", "-x509", "-newkey"] + args + ["-nodes", "-subj", "/CN=broker", "-days", "2",
                           "-keyout", key, "-out", crt], check=True, capture_output=True)
            fp = hashlib.sha256(ssl.PEM_cert_to_DER_cert(open(crt).read())).hexdigest()
            for ver in (ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3):
                ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                ctx.minimum_version = ctx.maximum_version = ver
                ctx.load_cert_chain(crt, key)
                b = Broker(ctx)
                p = start(b, "tls=" + fp)
                m = b.get(10, "tls")
                ok = b.get(10, "connect") and a_round(b, "testswitch")
                check(ok and m, "over %s, a server with %s" % (m and m[1], kind))
                p.kill()
        b = Broker(ctx)
        p = start(b, "tls=" + ":".join(fp[i:i + 2] for i in range(0, 64, 2)).upper())
        check(b.get(10, "connect") is not None, "the fingerprint with colons")
        p.kill()
        b = Broker(ctx)
        p = start(b, "tls=" + fp[:-1] + ("0" if fp[-1] != "0" else "1"))
        m = b.get(8, "gone")
        check(m and b.q.empty(), "another certificate: the handshake fails, nothing is sent (%s)" % (m and m[1]))
        p.kill()
        check(subprocess.run([sys.argv[1], "1", "tls=" + fp[:-2]], timeout=5).returncode == 2, "a short fingerprint is refused")
    print("FAILED" if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
