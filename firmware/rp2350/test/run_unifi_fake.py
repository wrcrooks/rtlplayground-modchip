#!/usr/bin/env python3
"""unifi.c (test_unifi_net, built with WATCH_S=20) against a scripted controller, no Docker:
what a real controller rarely sends, or sends only after a while.

    python3 run_unifi_fake.py build/test_unifi_net unifi

  - no URL set: http://unifi:8080/inform, then the controller DHCP option 43 names;
    the URL adopted through is kept, "inform auto" goes back, a name not found
  - the MAC table and LLDP neighbours in the inform, by UniFi port
  - no inform while the switch's ports can't be read, as while the bridge is down
  - adoption; then a new key from the controller, also once adopted
  - set-adopt with a name in its URL: taken; one that can't be used: not
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


def lldpdu(chassis_mac, port_id, ttl, name=None, ip=None):
    """An LLDPDU's TLVs, as hex for test_unifi_net's "lldp" command"""
    def tlv(t, v):
        return struct.pack(">H", t << 9 | len(v)) + v
    d = tlv(1, b"\x04" + chassis_mac) + tlv(2, b"\x05" + port_id) + tlv(3, struct.pack(">H", ttl))
    if name:
        d += tlv(5, name)
    if ip:
        d += tlv(8, bytes([5, 1]) + ip + bytes([2, 0, 0, 0, 1, 0]))
    return (d + tlv(0, b"")).hex()


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


def no_url(exe, cfg, store):
    """With no URL set: the default name, DHCP option 43, the URL kept on adoption"""
    macb = bytes([2, 0, 0x5e] + [random.randrange(256) for _ in range(3)])
    c, A = Controller(macb), os.urandom(16)
    sw = Switch(exe, c.port, ":".join("%02x" % b for b in macb), store, {"UNIFI_NO_URL": "1"})
    try:
        j, k = c.inform(15)
        st = sw.status()
        check(j and j.get("inform_url") == "http://unifi:8080/inform" and "RESOLVE unifi" in sw.lines
              and "inform URL http://unifi:8080/inform (the default)" in st and "unifi is 127.0.0.1" in st,
              "no URL set: http://unifi:8080/inform, the name looked up")
        sw.send("opt43 0104c0a8c8c8")
        check("inform URL http://192.168.200.200:8080/inform (from DHCP)" in sw.status(), "the longest address fits")
        sw.send("opt43 0202aabb01047f000001")
        st = sw.status()
        check("inform URL http://127.0.0.1:8080/inform (from DHCP)" in st, "DHCP option 43 names the controller")
        c.drain()
        j, k = c.inform(15)
        check(j and j.get("inform_url") == "http://127.0.0.1:8080/inform", "and is informed")
        c.keys = [A, DEFAULT]
        c.replies.put(({"_type": "setparam", "interval": 2, "mgmt_cfg": "authkey=%s\ncfgversion=v1\n" % A.hex(),
                        "system_cfg": cfg["adopted"]}, DEFAULT))
        got = sw.until(lambda l: "applied" in l, 20)
        st = sw.status()
        check(got and "inform URL http://127.0.0.1:8080/inform\n" in st + "\n", "adopted: that URL kept")
        sw.send("inform auto")
        check("(from DHCP)" in sw.status(), "inform auto: from DHCP again")
        sw.send("inform http://nowhere.example:8080/inform")
        got = sw.until(lambda l: l == "RESOLVE nowhere.example", 10)
        time.sleep(0.5)
        check(got is not None and "the controller's name not found" in sw.status(), "a name not found says so")
    finally:
        sw.stop()
        for i in (0, 1, 2):
            f = "%s.%d" % (store, i)
            os.path.exists(f) and os.unlink(f)


# the reply (REPLY_MAX) and its text unpacked (TEXT_MAX) at once; was 46 KB before both were opened
# in place and the request freed first
HEAP_BUDGET = 32 * 1024

def main():
    exe, cfgdir = sys.argv[1], sys.argv[2]
    cfg = {n: open(os.path.join(cfgdir, n + ".cfg")).read() for n in ("adopted", "ports")}
    macb = bytes([2, 0, 0x5e] + [random.randrange(256) for _ in range(3)])
    mac = ":".join("%02x" % b for b in macb)
    store = tempfile.NamedTemporaryFile(delete=False).name
    os.unlink(store)
    c = Controller(macb)
    A, B = os.urandom(16), os.urandom(16)
    no_url(exe, cfg, store + "x")
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

        # what's behind each port: the MAC table (read with "l2"), LLDP neighbours
        cm = "aa:" + mac[9:]	# the harness's clients
        pt = {p["port_idx"]: p for p in (j or {}).get("port_table", [])}
        mt = {i: [(m["mac"], m["vlan"], m["static"]) for m in pt.get(i, {}).get("mac_table", [])] for i in pt}
        check(mt.get(1) == [(cm + ":00:01", 1, False), (cm + ":00:02", 10, True)]
              and mt.get(4) == [(cm + ":00:04", 1, False)] and mt.get(10) == [(cm + ":00:06", 20, False)]
              and not mt.get(2) and not mt.get(9), "the MAC table by UniFi port (SFP+ front port 6 is 10): %s" % mt)
        check(not any(m in str(mt) for m in (mac, cm + ":00:05")), "not the switch's own, nor a LAG's")
        # the most the C heap has had in use: adoption, a configuration, an inform with the tables
        sw.send("heap")
        got = sw.until(lambda l: l.startswith("HEAP "), 5)
        peak = int(got[-1].split()[2]) if got else 0
        print("      heap peak %d bytes (%s)" % (peak, got and got[-1]))
        check(0 < peak <= HEAP_BUDGET, "the heap's peak is %d bytes, at most %d" % (peak, HEAP_BUDGET))
        sw.send("lldp 2 " + lldpdu(bytes.fromhex("0a0b0c0d0e0f"), b"ge-0/0/7", 120, b"core-sw", bytes([10, 0, 0, 2])))
        c.drain()
        j, k = c.inform(10)
        lt = (j or {}).get("lldp_table", [])
        check(lt == [{"local_port_idx": 2, "local_port_name": "Port 2", "is_wired": True, "chassis_id": "0a:0b:0c:0d:0e:0f",
                      "port_id": "ge-0/0/7", "system_name": "core-sw"}], "an LLDP neighbour on port 2: %s" % lt)
        sw.status()	# reads what's come so far
        check(sw.lines.count("L2 READ") == 1, "the MAC table read once a minute at most, not each inform: %d"
              % sw.lines.count("L2 READ"))
        sw.send("lldp 2 " + lldpdu(bytes.fromhex("0a0b0c0d0e0f"), b"ge-0/0/7", 0))
        c.drain()
        j, k = c.inform(10)
        check(j is not None and j.get("lldp_table") == [], "and gone when it says so (TTL 0)")

        # a new key, once adopted
        c.keys = [B, A, DEFAULT]
        c.replies.put(({"_type": "setparam", "mgmt_cfg": "authkey=%s\n" % B.hex()}, A))
        got = sw.until(lambda l: "new key" in l, 15)
        check(got is not None, "a new key from the controller is taken once adopted")
        c.drain()
        j, k = c.inform(10)
        check(k == B, "and used")

        # set-adopt with a name for the controller: taken, and looked up
        c.replies.put(({"_type": "cmd", "cmd": "set-adopt", "key": B.hex(), "uri": "http://unifi:8080/inform"}, B))
        got = sw.until(lambda l: "adopted by" in l, 15)
        check(got and "http://unifi:8080/inform" in got[-1], "set-adopt with a name: taken")
        c.drain()
        got = sw.until(lambda l: l == "RESOLVE unifi", 10)
        j, k = c.inform(15)
        check(got is not None and k == B, "the name looked up, informs go on")
        # and one that can't be used: not
        c.replies.put(({"_type": "cmd", "cmd": "set-adopt", "key": B.hex(), "uri": "https://unifi:8443/inform"}, B))
        got = sw.until(lambda l: "not usable" in l, 15)
        check(got is not None and "inform URL http://unifi:8080/inform\n" in sw.status() + "\n",
              "set-adopt with an https URL: not usable, the URL kept")

        # a configuration with no version is not applied
        c.replies.put(({"_type": "setparam", "system_cfg": cfg["ports"]}, B))
        got = sw.until(lambda l: "without a version" in l, 15)
        check(got is not None and not any(l.startswith("RUN ") for l in got), "a configuration without a version is not applied")

        # a damaged saved configuration is not used (struct saved: magic, cfgversion[24], then
        # struct unifi_sw: stp {int8 on, int8 rstp, int32 prio} = 8 bytes, then nvlans at 36)
        sw.stop()
        with open(store + ".2", "r+b") as f:
            check(f.read(4) == b"UNC2", "the configuration was saved")
            f.seek(36)
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
