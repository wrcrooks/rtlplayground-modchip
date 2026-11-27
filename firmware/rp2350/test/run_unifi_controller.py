#!/usr/bin/env python3
"""unifi.c (test_unifi_net) against a UniFi Network controller.

The controller: ghcr.io/jamesbraid/unifi-network:sim (UniFi Network 10.6.109,
login admin / admin), started here as unifi-test-controller unless UNIFI_API
names one already running. Its inform port is published on 127.0.0.1:UNIFI_INFORM.

    python3 run_unifi_controller.py build/test_unifi_net
"""
import http.cookiejar, json, os, queue, random, ssl, struct, subprocess, sys, tempfile, threading, time, urllib.request

IMAGE = "ghcr.io/jamesbraid/unifi-network:sim"
NAME = "unifi-test-controller"
API = os.environ.get("UNIFI_API")
INFORM = int(os.environ.get("UNIFI_INFORM", "0"))
fails = 0


def check(ok, what):
    global fails
    print(("PASS  " if ok else "FAIL  ") + what, flush=True)
    fails += not ok


def start_controller():
    global API, INFORM
    if API:
        return None
    subprocess.run(["docker", "rm", "-f", NAME], capture_output=True)
    subprocess.run(["docker", "run", "-d", "--name", NAME, "-p", "127.0.0.1:28443:8443", "-p", "127.0.0.1:28080:8080",
                    IMAGE], check=True, capture_output=True)
    API, INFORM = "https://127.0.0.1:28443", 28080
    for _ in range(120):
        r = subprocess.run(["docker", "inspect", "-f", "{{.State.Health.Status}}", NAME], capture_output=True, text=True)
        if r.stdout.strip() == "healthy":
            return NAME
        time.sleep(2)
    sys.exit("the controller didn't start")


class Controller:
    def __init__(self):
        ctx = ssl._create_unverified_context()
        self.op = urllib.request.build_opener(urllib.request.HTTPSHandler(context=ctx),
                                              urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
        self.call("/api/login", {"username": "admin", "password": "admin"})

    def call(self, path, body=None, method=None):
        r = urllib.request.Request(API + path, json.dumps(body).encode() if body is not None else None,
                                   {"Content-Type": "application/json"}, method=method)
        try:
            return json.loads(self.op.open(r, timeout=20).read())
        except urllib.error.HTTPError as e:
            return {"http": e.code, "body": e.read()[:300].decode(errors="replace")}

    def dev(self, mac):
        d = [x for x in self.call("/api/s/default/stat/device")["data"] if x["mac"] == mac]
        return d[0] if d else None

    def net(self, name, vlan):
        """The network with that VLAN, made if there's none; and whether it was made"""
        n = [x for x in self.call("/api/s/default/rest/networkconf")["data"] if x.get("vlan_enabled") and x.get("vlan") == vlan]
        if n:
            return n[0], False
        return self.call("/api/s/default/rest/networkconf", {"name": name, "purpose": "vlan-only", "vlan": vlan,
                                                              "vlan_enabled": True})["data"][0], True


class Switch:
    """test_unifi_net, with its output in a queue"""
    def __init__(self, exe, mac, store):
        self.p = subprocess.Popen([exe, str(INFORM), mac, store], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True, bufsize=1, env=dict(os.environ, UNIFI_PORTLED="1"))
        self.q = queue.Queue()
        self.lines = []
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
                l = self.q.get(timeout=0.5)
            except queue.Empty:
                continue
            if l is None:
                return None
            got.append(l)
            self.lines.append(l)
            if pred(l):
                return got
        return None

    def stop(self):
        self.p.kill()
        self.p.wait()


def wait(fn, timeout):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(2)
    return None


def runs(lines):
    return [l[4:] for l in lines or [] if l.startswith("RUN ")]


def main():
    exe = sys.argv[1]
    started = start_controller()
    mac = "02:00:5e:%02x:%02x:%02x" % (random.randrange(256), random.randrange(256), random.randrange(256))
    store = tempfile.NamedTemporaryFile(delete=False).name
    os.unlink(store)
    c = Controller()
    sw = Switch(exe, mac, store)
    try:
        d = wait(lambda: c.dev(mac), 60)
        check(d and not d.get("adopted") and d["model"] == "USWED36", "shows up waiting for adoption, as a USW Flex 2.5G 8")
        c.call("/api/s/default/cmd/devmgr", {"cmd": "adopt", "mac": mac})
        got = sw.until(lambda l: "applied" in l, 90)
        check(got and any("UniFi: adopted" in l for l in got), "adopted: the controller's key taken")
        r = runs(got)
        check("vlan 1 1 2 3 4 5 6" in r and "pvid 1 1" in r and "pvid 6 1" in r, "its configuration applied: %s" % r[:4])
        d = wait(lambda: (lambda d: d if d and d.get("state") == 1 and d.get("port_table") else None)(c.dev(mac)), 60)
        check(d is not None, "connected")
        pt = {p["port_idx"]: p for p in (d or {}).get("port_table", [])}
        check(pt.get(1, {}).get("up") and pt[1].get("speed") == 1000 and pt.get(4, {}).get("speed") == 2500
              and not pt.get(2, {}).get("up"), "port links as the switch has them")
        check(pt.get(9, {}).get("media") == "SFP+" and pt.get(1, {}).get("rx_bytes") == 1000000, "SFP+ ports and counters")

        # an LLDP neighbour on port 2 and the MAC table: kept by the controller
        tlv = lambda t, v: struct.pack(">H", t << 9 | len(v)) + v
        sw.send("lldp 2 " + (tlv(1, b"\x04\x0a\x0b\x0c\x0d\x0e\x0f") + tlv(2, b"\x05ge-0/0/7") + tlv(3, b"\x00\x78")
                             + tlv(5, b"core-sw") + tlv(0, b"")).hex())
        d = wait(lambda: (lambda d: d if d and d.get("lldp_table") else None)(c.dev(mac)), 40)
        lt = (d or {}).get("lldp_table", [])
        check(lt and lt[0].get("local_port_idx") == 2 and lt[0].get("chassis_id") == "0a:0b:0c:0d:0e:0f"
              and lt[0].get("port_id") == "ge-0/0/7", "the controller has the LLDP neighbour: %s" % lt)
        cm = "aa:" + mac[9:]	# the harness's clients, new for each switch
        want_sta = {cm + ":00:01": 1, cm + ":00:02": 1, cm + ":00:04": 4, cm + ":00:06": 10}
        clients = lambda: {x["mac"]: x.get("sw_port") for x in c.call("/api/s/default/stat/sta").get("data", [])
                           if x.get("mac", "").startswith(cm) and x.get("sw_mac") == mac}
        sta = wait(lambda: (lambda s: s if s == want_sta else None)(clients()), 40) or clients()
        check(sta == want_sta,
              "the MAC table's addresses are wired clients on their ports: %s" % sta)

        (v10, made10), (v20, made20) = c.net("UT10", 10), c.net("UT20", 20)
        others = [n["_id"] for n in c.call("/api/s/default/rest/networkconf")["data"]
                  if n["_id"] not in (v10["_id"], v20["_id"]) and n.get("purpose") != "wan"]
        if made10 or made20:
            got = sw.until(lambda l: "applied" in l, 60)
            r = runs(got)
        check(any(x.startswith("vlan 10 ") for x in r) and any(x.startswith("vlan 20 ") for x in r), "new networks become VLANs")
        c.call("/api/s/default/rest/device/" + d["_id"], {"port_overrides": [
            {"port_idx": 2, "name": "Camera", "native_networkconf_id": v10["_id"], "tagged_vlan_mgmt": "custom",
             "excluded_networkconf_ids": others},
            {"port_idx": 4, "setting_preference": "manual", "autoneg": False, "speed": 100, "full_duplex": True}]}, "PUT")
        got = sw.until(lambda l: "applied" in l, 60)
        r = runs(got)
        check("pvid 2 10" in r and "port 2 name Camera" in r and "port 4 100m full" in r, "port settings: %s" % r)
        check(any(x.split()[:2] == ["vlan", "1"] and "2" not in x.split()[2:] for x in r), "port 2 left out of VLAN 1")
        # isolation, storm control, an egress limit (the switch claims them in switch_caps)
        c.call("/api/s/default/rest/device/" + d["_id"], {"port_overrides": c.dev(mac)["port_overrides"] + [
            {"port_idx": 1, "isolation": True, "stormctrl_bcast_enabled": True, "stormctrl_bcast_rate": 100},
            {"port_idx": 3, "isolation": True, "egress_rate_limit_kbps_enabled": True, "egress_rate_limit_kbps": 5000}]}, "PUT")
        r = runs(sw.until(lambda l: "applied" in l, 60))
        check("isolate 1 2 4 5 6 0" in r and "isolate 3 2 4 5 6 0" in r and "storm 1 bcast 100 pps" in r
              and "bw out 3 00001388" in r, "isolation, storm control, egress limit: %s" % [x for x in r if x.split()[0] in ("isolate", "storm", "bw")])
        # Etherlighting (claimed, as the harness has a port LED board): the board's settings
        c.call("/api/s/default/rest/device/" + d["_id"], {"ether_lighting": {"mode": "network", "brightness": 40, "behavior": "steady"}}, "PUT")
        r = runs(sw.until(lambda l: "applied" in l, 60))
        check("portled mode vlan" in r and "portled brightness 40" in r and "portled breathe off" in r,
              "Etherlighting: %s" % [x for x in r if x.startswith("portled")])

        c.call("/api/s/default/cmd/devmgr", {"cmd": "set-locate", "mac": mac})
        check(sw.until(lambda l: l == "LED 1", 40) is not None, "locate flashes the system LED")
        c.call("/api/s/default/cmd/devmgr", {"cmd": "unset-locate", "mac": mac})
        check(sw.until(lambda l: l == "LED 3", 40) is not None, "and stops")
        c.call("/api/s/default/cmd/devmgr", {"cmd": "upgrade", "mac": mac})
        check(sw.until(lambda l: "upgrade refused" in l, 40) is not None, "a firmware upgrade is refused")
        d = wait(lambda: (lambda d: d if d and d.get("state") == 1 else None)(c.dev(mac)), 40)
        check(d is not None, "still connected after refusing it")

        # a restart: the key is kept, the configuration comes again
        sw.stop()
        sw = Switch(exe, mac, store)
        got = sw.until(lambda l: "restored" in l, 20)
        check(got and "pvid 2 10" in runs(got) and "port 4 100m full" in runs(got), "after a restart: the configuration from flash")
        d = wait(lambda: (lambda d: d if d and d.get("state") == 1 and d.get("uptime", 99) < 60 else None)(c.dev(mac)), 60)
        check(d is not None and not any("adopted" in l or "applied" in l for l in sw.lines[-20:]),
              "connected again with the same key, nothing sent again")
        if not got:
            sw.send("status")
            print("\n".join(sw.until(lambda l: l == "STATUS end", 20) or []))
            print(json.dumps({k: c.dev(mac).get(k) for k in ("state", "cfgversion", "adopted", "x_aes_gcm")}))

        # cut off after a change: back to the saved configuration, and that one is refused
        ov = [dict(o, name="Printer") if o["port_idx"] == 3 else o for o in c.dev(mac)["port_overrides"]]
        if not any(o["port_idx"] == 3 for o in ov):
            ov.append({"port_idx": 3, "name": "Printer"})
        c.call("/api/s/default/rest/device/" + d["_id"], {"port_overrides": ov}, "PUT")
        got = sw.until(lambda l: "applied" in l, 60)
        bad = [l for l in got or [] if "applied" in l][0].split()[3] if got else "?"
        sw.send("inform http://127.0.0.1:9/inform")
        got = sw.until(lambda l: l == "REBOOT", 60)
        check(got and any("controller lost" in l for l in got), "no controller after a change: restart")
        sw.stop()
        sw = Switch(exe, mac, store)
        got = sw.until(lambda l: "restored" in l, 20)
        check(got and bad not in got[-1], "restarted with the configuration of before")
        sw.send("inform http://127.0.0.1:8080/inform")
        wait(lambda: (lambda d: d and d.get("state") == 1)(c.dev(mac)), 60)
        c.call("/api/s/default/cmd/devmgr", {"cmd": "force-provision", "mac": mac})
        got = sw.until(lambda l: "not applied" in l, 60)
        check(got is not None and bad in got[-1] and not runs(got), "and configuration %s is refused afterwards" % bad)
        sw.send("status")
        got = sw.until(lambda l: l == "STATUS end", 20)
        print("\n".join(got or []))
        check(got and any("refusing configuration" in l for l in got), "the status says so")
    finally:
        sw.stop()
        c.call("/api/s/default/cmd/sitemgr", {"cmd": "delete-device", "mac": mac})
        for f in (store + ".0", store + ".2"):
            os.path.exists(f) and os.unlink(f)
        if started:
            subprocess.run(["docker", "rm", "-f", started], capture_output=True)
    print("%d failed" % fails)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
