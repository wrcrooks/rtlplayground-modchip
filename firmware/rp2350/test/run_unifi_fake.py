#!/usr/bin/env python3
"""unifi.c (test_unifi_net, built with WATCH_S=20) against a scripted controller, no Docker:
what a real controller rarely sends, or sends only after a while.

    python3 run_unifi_fake.py build/test_unifi_net unifi

  - no inform while the switch's ports can't be read, as while the bridge is down
  - adoption; then a new key from the controller, also once adopted
  - set-adopt with a name in its URL: kept as it was, informs go on
  - a configuration without a version: not applied
  - a damaged saved configuration: not used
  - forgotten while a change is being checked: no restart
"""
import hashlib, json, os, queue, random, socket, struct, subprocess, sys, tempfile, threading, time, zlib
from cryptography.hazmat.primitives import padding
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

DEFAULT = hashlib.md5(b"ubnt").digest()
fails = 0


def check(ok, what):
    global fails
    print(("PASS  " if ok else "FAIL  ") + what, flush=True)
    fails += not ok


def unseal(pkt, keys):
    """The JSON in an inform, and the key it was sealed with"""
    flags, iv, n = struct.unpack(">H", pkt[14:16])[0], pkt[16:32], struct.unpack(">I", pkt[36:40])[0]
    body = pkt[40:40 + n]
    for k in keys:
        try:
            if flags & 8:
                d = AESGCM(k).decrypt(iv, body, pkt[:40])
            else:
                d = Cipher(algorithms.AES(k), modes.CBC(iv)).decryptor()
                d = d.update(body) + d.finalize()
                u = padding.PKCS7(128).unpadder()
                d = u.update(d) + u.finalize()
            if flags & 2:
                d = zlib.decompress(d)
            return json.loads(d), k
        except Exception:
            continue
    return None, None


def seal(obj, mac, key):
    iv = os.urandom(16)
    p = padding.PKCS7(128).padder()
    d = p.update(zlib.compress(json.dumps(obj).encode())) + p.finalize()
    e = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor()
    d = e.update(d) + e.finalize()
    return b"TNBU" + struct.pack(">I", 0) + mac + struct.pack(">H", 3) + iv + struct.pack(">II", 1, len(d)) + d


class Controller:
    """Answers informs: the next queued reply, sealed with the key given, else nothing"""
    def __init__(self, mac):
        self.mac, self.keys = mac, [DEFAULT]
        self.replies, self.informs = queue.Queue(), queue.Queue()
        self.s = socket.socket()
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.s.bind(("127.0.0.1", 0))
        self.s.listen(4)
        self.port = self.s.getsockname()[1]
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            c, _ = self.s.accept()
            try:
                self.one(c)
            except Exception as e:
                print("controller:", e)
            c.close()

    def one(self, c):
        data = b""
        while b"\r\n\r\n" not in data:
            data += c.recv(4096)
        head, body = data.split(b"\r\n\r\n", 1)
        n = int([l.split(b":")[1] for l in head.split(b"\r\n") if l.lower().startswith(b"content-length")][0])
        while len(body) < n:
            body += c.recv(4096)
        j, k = unseal(body, self.keys)
        self.informs.put((j, k))
        try:
            obj, key = self.replies.get_nowait()
            out = seal(obj, self.mac, key)
        except queue.Empty:
            out = b""
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/x-binary\r\nContent-Length: %d\r\n\r\n" % len(out) + out)

    def inform(self, timeout):
        try:
            return self.informs.get(timeout=timeout)
        except queue.Empty:
            return None, None

    def drain(self):
        while not self.informs.empty():
            self.informs.get()


class Switch:
    def __init__(self, exe, port, mac, store, env=None):
        self.p = subprocess.Popen([exe, str(port), mac, store], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True, bufsize=1, env=dict(os.environ, **(env or {})))
        self.q, self.lines = queue.Queue(), []
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for l in self.p.stdout:
            self.q.put(l.rstrip("\n"))
        self.q.put(None)

    def send(self, line):
        self.p.stdin.write(line + "\n")

    def until(self, pred, timeout):
        """The lines up to one pred() takes, or None"""
        end, got = time.time() + timeout, []
        while time.time() < end:
            try:
                l = self.q.get(timeout=0.3)
            except queue.Empty:
                continue
            if l is None:
                return None
            got.append(l)
            self.lines.append(l)
            if pred(l):
                return got
        return None

    def _gather(self, secs):
        """Every line in that many seconds"""
        end, got = time.time() + secs, []
        while time.time() < end:
            try:
                l = self.q.get(timeout=0.3)
            except queue.Empty:
                continue
            if l is None:
                break
            got.append(l)
            self.lines.append(l)
        return got

    def status(self):
        self.send("status")
        return "\n".join(self.until(lambda l: l == "STATUS end", 10) or [])

    def stop(self):
        self.p.kill()
        self.p.wait()


def main():
    exe, cfgdir = sys.argv[1], sys.argv[2]
    cfg = {n: open(os.path.join(cfgdir, n + ".cfg")).read() for n in ("adopted", "ports")}
    macb = bytes([2, 0, 0x5e] + [random.randrange(256) for _ in range(3)])
    mac = ":".join("%02x" % b for b in macb)
    store = tempfile.NamedTemporaryFile(delete=False).name
    os.unlink(store)
    c = Controller(macb)
    A, B = os.urandom(16), os.urandom(16)
    sw = Switch(exe, c.port, mac, store, {"UNIFI_PORTS": "0"})
    try:
        # no ports read yet: no inform, rather than one with no model
        j, _ = c.inform(4)
        check(j is None and "waiting for the switch's ports" in sw.status(), "no inform while the ports can't be read")
        sw.send("ports 6")
        j, k = c.inform(15)
        check(j and j.get("model") == "USWED36" and j.get("model_display") == "USW Flex 2.5G 8" and k == DEFAULT,
              "then informs, as a USW Flex 2.5G 8: %s" % (j and j.get("model")))

        # adopted
        c.keys = [A, DEFAULT]
        c.replies.put(({"_type": "setparam", "interval": 2, "mgmt_cfg": "authkey=%s\ncfgversion=v1\n" % A.hex(),
                        "system_cfg": cfg["adopted"]}, DEFAULT))
        got = sw.until(lambda l: "applied" in l, 20)
        check(got and any("UniFi: adopted" in l for l in got) and any(l.startswith("RUN vlan 1 ") for l in got),
              "adopted, configuration v1 applied")
        c.drain()
        j, k = c.inform(10)
        check(k == A and j and j.get("cfgversion") == "v1", "informs with the key given, as v1")

        # a new key, once adopted
        c.keys = [B, A, DEFAULT]
        c.replies.put(({"_type": "setparam", "mgmt_cfg": "authkey=%s\n" % B.hex()}, A))
        got = sw.until(lambda l: "new key" in l, 15)
        check(got is not None, "a new key from the controller is taken once adopted")
        c.drain()
        j, k = c.inform(10)
        check(k == B, "and used")

        # set-adopt with a name for the controller: no DNS here, so the URL stays
        c.replies.put(({"_type": "cmd", "cmd": "set-adopt", "key": B.hex(), "uri": "http://unifi:8080/inform"}, B))
        got = sw.until(lambda l: "not usable" in l, 15)
        check(got is not None, "set-adopt with a name: not usable")
        check("inform URL http://127.0.0.1:8080/inform" in sw.status(), "the URL kept")
        c.drain()
        j, k = c.inform(10)
        check(k == B, "informs go on")

        # a configuration with no version is not applied
        c.replies.put(({"_type": "setparam", "system_cfg": cfg["ports"]}, B))
        got = sw.until(lambda l: "without a version" in l, 15)
        check(got is not None and not any(l.startswith("RUN ") for l in got), "a configuration without a version is not applied")

        # a damaged saved configuration is not used (struct saved: magic, cfgversion[24], then nvlans)
        sw.stop()
        with open(store + ".2", "r+b") as f:
            check(f.read(4) == b"UNC1", "the configuration was saved")
            f.seek(28)
            f.write(bytes([200]))
        sw = Switch(exe, c.port, mac, store)
        got = sw._gather(3)
        check(any("damaged" in l for l in got) and not any(l.startswith("RUN ") or "restored" in l for l in got),
              "a damaged saved configuration is not used")

        # forgotten while a change is checked: no restart when the check runs out
        c.drain()
        c.inform(15)
        c.replies.put(({"_type": "setparam", "interval": 2, "cfgversion": "v2", "system_cfg": cfg["ports"]}, B))
        got = sw.until(lambda l: "applied" in l, 15)
        check(got and "v2" in got[-1], "configuration v2 applied")
        sw.send("inform http://127.0.0.1:9/inform")
        sw.send("forget")
        got = sw._gather(27)
        check(any("forgotten" in l for l in got) and "REBOOT" not in got and sw.p.poll() is None,
              "forgotten while it was checked: no restart")
    finally:
        sw.stop()
        for i in (0, 1, 2):
            f = "%s.%d" % (store, i)
            os.path.exists(f) and os.unlink(f)
    print("%d failed" % fails)
    sys.exit(1 if fails else 0)


main()
