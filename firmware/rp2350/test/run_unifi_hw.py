#!/usr/bin/env python3
"""UniFi on a real switch with the mod chip, against a UniFi Network controller
in Docker: run_unifi_controller.py's checks, on hardware.

    python3 run_unifi_hw.py <switch address> [--key update.pem] [options]

1. Builds the firmware with the update key, signs it and sends it to the switch
   (POST /update), then waits for the new version and keeps it (unless --no-flash).
2. Starts the controller (or uses UNIFI_API / UNIFI_INFORM), and relays its inform
   port from this host's address on the switch's network.
3. Points the switch at it, adopts it, and changes VLANs, port settings, STP,
   isolation, storm control and egress limits through the controller's API. Each
   must reach the switch with every command taken; this host must stay reachable.
4. Locate, a refused upgrade, a restart (configuration from flash), and the
   cut-off guard (unless --quick): an unreachable controller after a change
   restarts the mod chip with the configuration before, which is refused after.
5. Puts the switch back: forgotten, its UniFi settings as they were, the switch
   reset to its saved configuration, the command log cleared.

Only a switch whose mod chip says it is that board (--board) is tested, and only
ports with no link are changed, never this host's; MODCHIP_HW_REFUSE lists
addresses to refuse anyway. The password comes from
MODCHIP_PASSWORD, else it is asked for. An LLDP frame is sent from this host only
with CAP_NET_RAW (root); otherwise that check is skipped.
"""
import argparse, getpass, http.client, json, os, random, socket, struct, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import run_unifi_controller as rc  # noqa: E402

REFUSE = set(os.environ.get("MODCHIP_HW_REFUSE", "").split())	# addresses never to test on
fails = skips = 0


class Stop(Exception):
    """No point going on"""


def check(ok, what):
    global fails
    print(("PASS  " if ok else "FAIL  ") + what, flush=True)
    fails += not ok
    return ok


def skip(what):
    global skips
    print("SKIP  " + what, flush=True)
    skips += 1


class Switch:
    """The switch's web interface: login, /cmd, the JSON pages, /update"""
    def __init__(self, host, password, https=False):
        self.host, self.password, self.https, self.cookie = host, password, https, None

    def _conn(self, timeout):
        if self.https:
            import ssl
            return http.client.HTTPSConnection(self.host, timeout=timeout, context=ssl._create_unverified_context())
        return http.client.HTTPConnection(self.host, timeout=timeout)

    def _req(self, method, path, body=None, ctype="text/plain", timeout=20):
        c = self._conn(timeout)
        h = {"Content-Type": ctype} if body is not None else {}
        if self.cookie:
            h["Cookie"] = self.cookie
        c.request(method, path, body, h)
        r = c.getresponse()
        data = r.read()
        c.close()
        return r.status, r.getheader("Location") or "", r.getheader("Set-Cookie") or "", data

    def login(self):
        st, _, ck, _ = self._req("POST", "/login", "pwd=" + self.password, "application/x-www-form-urlencoded")
        if "session=" not in ck:
            raise RuntimeError("login refused (HTTP %d): wrong password?" % st)
        self.cookie = ck.split(";")[0]

    def request(self, method, path, body=None, ctype="text/plain", timeout=20):
        for _ in range(2):
            if not self.cookie:
                self.login()
            st, loc, _, data = self._req(method, path, body, ctype, timeout)
            if st == 302 and "login" in loc:
                self.cookie = None	# a restart forgets sessions
                continue
            return st, data
        raise RuntimeError("not logged in")

    def cmd(self, line):
        st, data = self.request("POST", "/cmd", line)
        return data.decode(errors="replace")

    def get(self, path):
        st, data = self.request("GET", path)
        return json.loads(data) if st == 200 else None

    def up(self, timeout=3):
        try:
            c = self._conn(timeout)
            c.request("GET", "/login.html")
            ok = c.getresponse().status == 200
            c.close()
            return ok
        except OSError:
            return False


class Events:
    """The switch's events, each seen once, across restarts of the mod chip"""
    def __init__(self, sw):
        self.sw, self.seen, self.uptime, self.restarts = sw, set(), 0, 0

    def new(self):
        try:
            f = self.sw.get("/features.json") or {}
            if f.get("uptime", 0) < self.uptime:
                self.seen = set()	# restarted: the sequence begins again, the events before are gone
                self.restarts += 1
                print("      (the mod chip restarted)", flush=True)
            self.uptime = f.get("uptime", 0)
            ev = self.sw.get("/events.json") or []
        except (OSError, RuntimeError, ValueError):
            return []
        out = [e["text"] for e in ev if e["seq"] not in self.seen]
        self.seen.update(e["seq"] for e in ev)
        return out

    def until(self, pred, timeout):
        """The new events up to one pred() takes, or None"""
        end, got = time.time() + timeout, []
        while time.time() < end:
            for t in self.new():
                print("      event: " + t, flush=True)
                got.append(t)
                if pred(t):
                    return got
            time.sleep(2)
        return None


class Relay:
    """TCP from (address, port) to 127.0.0.1:to"""
    def __init__(self, address, port, to):
        self.s = socket.socket()
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.s.bind((address, port))
        self.s.listen(8)
        self.to = to
        threading.Thread(target=self.accept, daemon=True).start()

    def accept(self):
        while True:
            try:
                a, _ = self.s.accept()
            except OSError:
                return
            try:
                b = socket.create_connection(("127.0.0.1", self.to), timeout=10)
            except OSError:
                a.close()
                continue
            for x, y in ((a, b), (b, a)):
                threading.Thread(target=self.pump, args=(x, y), daemon=True).start()

    @staticmethod
    def pump(x, y):
        try:
            while True:
                d = x.recv(65536)
                if not d:
                    break
                y.sendall(d)
        except OSError:
            pass
        for s in (x, y):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def close(self):
        self.s.close()


def route_to(ip):
    """This host's address and interface towards ip"""
    r = subprocess.run(["ip", "-o", "route", "get", ip], capture_output=True, text=True).stdout.split()
    return r[r.index("src") + 1], r[r.index("dev") + 1]


def lldpdu(src_mac):
    tlv = lambda t, v: struct.pack(">H", t << 9 | len(v)) + v
    return (bytes.fromhex("0180c200000e") + src_mac + b"\x88\xcc" + tlv(1, b"\x04" + src_mac)
            + tlv(2, b"\x05unifi-hw-test") + tlv(3, b"\x00\x78") + tlv(5, b"unifi-hw-test") + tlv(0, b""))


def build_and_sign(board, key, out_dir):
    b = os.path.join(out_dir, "build-hw-" + board)
    sdk = os.environ.get("PICO_SDK_PATH") or os.path.expanduser("~/Desktop/Workspace/pico-sdk-2.3.1")
    for c in (["cmake", "-S", ROOT, "-B", b, "-DPICO_SDK_PATH=" + sdk, "-DMODCHIP_BOARD=" + board,
               "-DMODCHIP_UPDATE_KEY=" + os.path.abspath(key)], ["cmake", "--build", b, "-j8"]):
        r = subprocess.run(c, capture_output=True, text=True)
        if r.returncode:
            sys.exit("build failed:\n" + (r.stdout + r.stderr)[-3000:])
    binf, mcu = os.path.join(b, "modchip.bin"), os.path.join(b, "modchip-hw-test.mcu")
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "sign_update.py"), "sign", key, binf, mcu],
                   check=True, capture_output=True)
    data = open(binf, "rb").read()
    i = data.index(b"MCVERSION:") + 10
    return mcu, data[i:data.index(b"\0", i)].decode()


def flash(sw, mcu, version):
    print("      sending %s (%s)" % (os.path.basename(mcu), version), flush=True)
    st, body = sw.request("POST", "/update", open(mcu, "rb").read(), "application/octet-stream", timeout=200)
    if not check(st == 200 and b"rror" not in body, "the switch takes the update: %s" % body.decode(errors="replace").strip()[:120]):
        return False
    time.sleep(10)
    end, u = time.time() + 180, {}
    while time.time() < end:
        try:
            u = (sw.get("/features.json") or {}).get("update", {})
            if u.get("version") == version:
                break
        except (OSError, RuntimeError, ValueError):
            pass
        time.sleep(3)
    if not check(u.get("version") == version, "runs %s, slot %s, %s" % (u.get("version"), u.get("slot"),
                                                                       "on trial" if u.get("trial") else "kept")):
        return False
    if u.get("trial"):
        sw.cmd("update keep")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("switch", help="its address[:port]")
    ap.add_argument("--key", default=os.environ.get("MODCHIP_UPDATE_KEY"), help="the update key (.pem)")
    ap.add_argument("--board", default="beetle")
    ap.add_argument("--no-flash", action="store_true", help="test the firmware the switch has")
    ap.add_argument("--quick", action="store_true", help="no restart, no cut-off check (they take ~6 min)")
    ap.add_argument("--relay-port", type=int, default=8088, help="where the switch reaches the controller on this host")
    ap.add_argument("--https", action="store_true", help="the switch's web interface over HTTPS")
    a = ap.parse_args()
    if a.switch.split(":")[0] in REFUSE:
        sys.exit("%s is in MODCHIP_HW_REFUSE: not that one" % a.switch)
    if not a.no_flash and not a.key:
        sys.exit("--key (or MODCHIP_UPDATE_KEY) is needed to flash, or --no-flash")
    password = os.environ.get("MODCHIP_PASSWORD") or getpass.getpass("The switch's web password: ")
    sw = Switch(a.switch, password, a.https)
    if not sw.up():
        sys.exit("%s doesn't answer" % a.switch)
    sw.login()
    # only a switch with the mod chip, of this board: nothing is sent to any other
    try:
        board = (sw.get("/features.json") or {}).get("update", {}).get("board", "")
    except ValueError:
        board = ""
    if not board.startswith(a.board + " "):
        sys.exit("%s isn't a %s mod chip (board %r): not testing it" % (a.switch, a.board, board))
    host_ip, dev = route_to(a.switch.split(":")[0])
    sw_mac = (sw.get("/information.json") or {}).get("mac_address", "").lower()
    host_mac = open("/sys/class/net/%s/address" % dev).read().strip()
    print("      this host: %s on %s (%s)" % (host_ip, dev, host_mac), flush=True)

    if not a.no_flash:
        mcu, version = build_and_sign(a.board, a.key, HERE)
        if not flash(sw, mcu, version):
            sys.exit(1)

    f = sw.get("/features.json")
    before = f["services"].get("unifi")
    if before is None:
        sys.exit("this firmware has no UniFi")
    try:
        relay = Relay(host_ip, a.relay_port, 0)
    except OSError as e:
        sys.exit("can't listen on %s:%d (%s): pick another with --relay-port" % (host_ip, a.relay_port, e.strerror))
    started = rc.start_controller()
    relay.to = rc.INFORM
    c = rc.Controller()
    ev = Events(sw)
    ev.new()
    mac = dev_id = None
    made = []
    try:
        sw.cmd("unifi forget")
        sw.cmd("unifi inform http://%s:%d/inform" % (host_ip, a.relay_port))
        sw.cmd("unifi on")
        d = rc.wait(lambda: c.dev(sw_mac), 60)
        if not check(d is not None and d["model"] in ("USWED35", "USWED36"),
                     "shows up waiting for adoption, as %s" % (d and d["model"])):
            print(sw.cmd("unifi"))
            raise Stop
        mac = d["mac"]
        c.call("/api/s/default/cmd/devmgr", {"cmd": "adopt", "mac": mac})
        got = ev.until(lambda t: "applied" in t, 90)
        check(got and any("UniFi: adopted" in t for t in got), "adopted: the controller's key taken")
        check(got and "with errors" not in got[-1], "its configuration taken, every command")
        d = rc.wait(lambda: (lambda d: d if d and d.get("state") == 1 and d.get("port_table") else None)(c.dev(mac)), 60)
        if not check(d is not None, "connected"):
            raise Stop
        dev_id = d["_id"]
        pt = {p["port_idx"]: p for p in (d or {}).get("port_table", [])}
        print("      ports: " + ", ".join("%d %s" % (i, "%d M" % p.get("speed") if p.get("up") else "down")
                                         for i, p in sorted(pt.items())), flush=True)

        # this host as a wired client on its port; the ports to change: no link, not this host's
        sta = rc.wait(lambda: [x for x in c.call("/api/s/default/stat/sta").get("data", [])
                               if x.get("mac") == host_mac and x.get("sw_mac") == mac], 150)
        mine = sta[0].get("sw_port") if sta else None
        check(mine is not None and pt.get(mine, {}).get("up"), "this host is a wired client on port %s, which is up" % mine)
        free = [i for i, p in sorted(pt.items()) if not p.get("up") and i != mine and p.get("media") != "SFP+"]
        if len(free) < 2:
            free = [i for i, p in sorted(pt.items()) if not p.get("up") and i != mine]
        if not check(len(free) >= 2, "two ports with no link to change: %s" % free):
            raise Stop
        pa, pb = free[0], free[1]
        print("      changing ports %d and %d" % (pa, pb), flush=True)

        # an LLDP neighbour: this host
        sw.cmd("lldp on")
        try:
            s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
            s.bind((dev, 0))
            frame = lldpdu(bytes.fromhex(host_mac.replace(":", "")))
            s.send(frame)
            d = rc.wait(lambda: (s.send(frame), (lambda d: d if d and d.get("lldp_table") else None)(c.dev(mac)))[1], 40)
            lt = (d or {}).get("lldp_table", [])
            check(lt and lt[0].get("local_port_idx") == mine and lt[0].get("chassis_id") == host_mac,
                  "the controller has this host as an LLDP neighbour: %s" % lt)
            s.close()
        except PermissionError:
            skip("LLDP: sending a frame needs CAP_NET_RAW (root); 'neighbors' on the switch shows what it heard")

        def change(body, what, timeout=60):
            c.call("/api/s/default/rest/device/" + dev_id, body, "PUT")
            got = ev.until(lambda t: "applied" in t, timeout)
            check(got and "with errors" not in got[-1], what + ": taken, every command")
            check(sw.up(), "  this host still reaches the switch")
            return got

        (v10, m10), (v20, m20) = c.net("UT10", 10), c.net("UT20", 20)
        made += [n["_id"] for n, m in ((v10, m10), (v20, m20)) if m]
        if made:
            ev.until(lambda t: "applied" in t, 60)
        others = [n["_id"] for n in c.call("/api/s/default/rest/networkconf")["data"]
                  if n["_id"] not in (v10["_id"], v20["_id"]) and n.get("purpose") != "wan"]
        ov = [{"port_idx": pa, "name": "Camera", "native_networkconf_id": v10["_id"], "tagged_vlan_mgmt": "custom",
               "excluded_networkconf_ids": others},
              {"port_idx": pb, "setting_preference": "manual", "autoneg": False, "speed": 100, "full_duplex": True}]
        change({"port_overrides": ov}, "port %d: VLAN 10 untagged, named; port %d: 100M full" % (pa, pb))
        v = sw.get("/vlan.json?vid=10") or {}
        pv = int(v.get("pvid", "0x0"), 16)
        check(bin(pv).count("1") == 1, "VLAN 10 is one port's PVID on the switch: %s" % v)
        ov[0].update(isolation=True, stormctrl_bcast_enabled=True, stormctrl_bcast_rate=100)
        ov[1].update(isolation=True, egress_rate_limit_kbps_enabled=True, egress_rate_limit_kbps=5000)
        change({"port_overrides": ov}, "isolation, storm control, an egress limit")
        print("      " + sw.cmd("isolate %d show" % pa).strip().replace("\n", "\n      "), flush=True)
        change({"stp_priority": "4096"}, "STP bridge priority 4096")

        c.call("/api/s/default/cmd/devmgr", {"cmd": "set-locate", "mac": mac})
        if not check(rc.wait(lambda: "locating" in sw.cmd("unifi"), 40), "locate: the system LED flashes"):
            print(sw.cmd("unifi"))
        c.call("/api/s/default/cmd/devmgr", {"cmd": "unset-locate", "mac": mac})
        check(rc.wait(lambda: "locating" not in sw.cmd("unifi"), 40), "and stops")
        c.call("/api/s/default/cmd/devmgr", {"cmd": "upgrade", "mac": mac})
        check(ev.until(lambda t: "upgrade refused" in t, 40) is not None, "a firmware upgrade is refused")

        if a.quick:
            skip("a restart and the cut-off guard (--quick)")
            raise Stop
        # a restart of the mod chip: the configuration from flash, the same key
        sw.cmd("reboot in 1")
        got = ev.until(lambda t: "restored" in t, 150)
        check(got and "with errors" not in got[-1], "after a restart: the configuration from flash")
        d = rc.wait(lambda: (lambda d: d if d and d.get("state") == 1 and d.get("uptime", 999) < 120 else None)(c.dev(mac)), 90)
        later = ev.until(lambda t: False, 10) or []
        check(d is not None and not any("adopted" in t or "applied" in t for t in later),
              "connected again with the same key, nothing sent again")

        # cut off after a change: back to the configuration before, which is refused afterwards
        ov[0]["name"] = "Printer"
        got = change({"port_overrides": ov}, "a new port name")
        bad = got[-1].split()[2] if got else "?"
        sw.cmd("unifi inform http://%s:9/inform" % host_ip)
        restarts = ev.restarts
        # "controller lost" is logged as it restarts, so it's gone with the restart: the uptime tells
        got = ev.until(lambda t: "restored" in t and ev.restarts > restarts, 300)
        check(ev.restarts > restarts, "no controller after a change: the mod chip restarts")
        check(got and bad not in got[-1], "with the configuration of before: %s" % (got and got[-1]))
        sw.cmd("unifi inform http://%s:%d/inform" % (host_ip, a.relay_port))
        rc.wait(lambda: (lambda d: d and d.get("state") == 1)(c.dev(mac)), 90)
        c.call("/api/s/default/cmd/devmgr", {"cmd": "force-provision", "mac": mac})
        got = ev.until(lambda t: "not applied" in t, 90)
        check(got is not None and bad in got[-1], "and configuration %s is refused afterwards" % bad)
        st = sw.cmd("unifi")
        if not check("refusing configuration" in st, "the status says so"):
            print(st)
    except Stop:
        pass
    finally:
        print("      putting the switch back", flush=True)
        try:
            sw.cmd("unifi forget")
            sw.cmd("unifi inform " + (before.get("url") or "auto"))
            sw.cmd("unifi " + ("on" if before.get("on") else "off"))
            try:
                sw.cmd("reset")		# the switch's saved configuration again; it may not answer
            except OSError:
                pass
            rc.wait(lambda: sw.up(), 60)
            time.sleep(5)
            sw.request("GET", "/cmd_log_clear")
        except (OSError, RuntimeError) as e:
            print("      could not put it all back: %r" % e)
        if mac:
            c.call("/api/s/default/cmd/sitemgr", {"cmd": "delete-device", "mac": mac})
        for n in made:
            c.call("/api/s/default/rest/networkconf/" + n, None, "DELETE")
        relay.close()
        if started:
            subprocess.run(["docker", "rm", "-f", started], capture_output=True)
        print("%d failed, %d skipped" % (fails, skips))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
