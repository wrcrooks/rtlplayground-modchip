#!/usr/bin/env python3
"""The dashboard (web/dashboard.html) with simulated data, for working on it
without a switch: six ports with changing traffic, an hour of history, an
SFP module, link changes and protocol events.

  python3 tools/dashboard_preview.py [port]     then open http://localhost:8080/dashboard.html

/metrics.json and /history.bin follow the layouts of src/metrics.c.
"""
import http.server
import json
import math
import pathlib
import random
import struct
import sys

WEB = pathlib.Path(__file__).resolve().parent.parent / "web"
UP_AT_START = 4000                  # the "switch" has been up a while
polls = 0


def now():
    """Simulated uptime: 2 s further on each poll, so the page sees steady
    rates whatever its clock does (headless browsers run on virtual time)"""
    return UP_AT_START + 2 * polls
PERIOD = 10
NAMES = ["uplink", "NAS", "Port 3", "Port 4", "SFP+ 1", "SFP+ 2"]
LINK = [6, 3, 3, 0, 5, 3]           # 1 + speed code: 2.5G, 1G, 1G, down, 10G, 1G
MAX = [2500, 2500, 2500, 2500, 10000, 10000]


def rates(t, i):
    """bits/s in and out on port i at uptime t"""
    base = [300e6, 120e6, 15e6, 0, 900e6, 40e6][i]
    if not LINK[i]:
        return 0.0, 0.0
    wave = 1 + 0.6 * math.sin(t / (90 + 40 * i)) + 0.3 * math.sin(t / 17 + i)
    burst = 2.5 if (int(t) // 120 + i) % 7 == 0 else 1
    rx = max(0.0, base * wave * burst)
    return rx, rx * (0.35 + 0.1 * i)


def counters(t, i):
    """cumulative counters, by integrating the rates in 1 s steps"""
    rx = tx = 0.0
    step = 1
    for s in range(0, int(t), step):
        a, b = rates(s, i)
        rx += a * step
        tx += b * step
    rxo, txo = int(rx / 8), int(tx / 8)
    rxp, txp = rxo // 900, txo // 900
    errs = int(t / 50) if i == 2 else 0
    return [rxo, txo, int(rxp * .97), int(rxp * .02), int(rxp * .01), int(txp * .98), int(txp * .015), int(txp * .005),
            int(t / 300), 0, errs, 0]


def metrics():
    global polls
    polls += 1
    t = now()
    ports = []
    for i in range(6):
        link = LINK[i]
        if i == 3 and int(t) % 60 < 20:
            link = 3                       # port 4 flaps
        ports.append({"n": i + 1, "name": NAMES[i], "en": True, "sfp": i >= 4, "max": MAX[i], "link": link,
                      "mbps": [0, 10, 100, 1000, 500, 10000, 2500, 5000][link], "c": counters(t, i)})
    ev = []
    for k in range(int(t) // 60 - 3, int(t) // 60 + 1):
        ev.append({"seq": 2 * k + 1, "t": int((k * 60) * 1000), "port": 4, "link": 3, "mbps": 1000})
        ev.append({"seq": 2 * k + 2, "t": int((k * 60 + 20) * 1000), "port": 4, "link": 0, "mbps": 0})
    ev = [e for e in ev if e["t"] <= t * 1000]
    return {
        "t": int(t * 1000), "uptime": int(t), "period": PERIOD, "bridge_up": True, "ports": ports,
        "switch": {"temp": 48 + 3 * math.sin(t / 400), "mac": 40 + int(10 * math.sin(t / 200))},
        "stp": {"on": True, "root_port": 255, "tc": int(t) // 240, "root_changes": 1},
        "lacp": {"on": True, "up": 2, "changes": int(t) // 900},
        "igmp": {"on": True, "groups": 3 + int(2 * math.sin(t / 150)), "joins": int(t) // 20, "leaves": int(t) // 25},
        "sfp": [{"slot": 1, "present": True, "los": False, "vendor": "FS", "model": "SFP-10GSR-85", "ddm": True,
                 "temp": 36 + math.sin(t / 300), "vcc": 3.31, "bias": 6.2 + .1 * math.sin(t / 50),
                 "tx_uw": 520 + 10 * math.sin(t / 70), "rx_uw": 260 + 30 * math.sin(t / 90)},
                {"slot": 2, "present": False, "los": True, "vendor": "", "model": "", "ddm": False}],
        "modchip": {"load": 8 + 6 * abs(math.sin(t / 30)) + random.random(), "heap_free": 190000 + random.randint(0, 9000),
                    "heap_total": 276000, "temp": 39 + math.sin(t / 500)},
        "bridge": {"link_load": 12 + 8 * abs(math.sin(t / 40)), "frames_ps": 25, "requests": int(t * 30),
                   "timeouts": int(t) // 700, "link_errors": 0, "events_dropped": 0, "rx_overruns": 0, "resets": 0,
                   "rx_frames": int(t * 25), "tx_frames": int(t * 20), "uart_rx": int(t * 9000),
                   "uart_tx": int(t * 4000), "uptime": int(t), "rx_dropped": 0, "bridge_link_errors": 0},
        "t3": {"up": True, "enabled": True, "khz": 400, "reads": int(t * 25), "writes": int(t * 3), "errors": 0},
        "events": ev,
    }


def history():
    up = int(now())
    first = up - 3600 + PERIOD
    out = bytearray()
    n = 0
    for ts in range(first - first % PERIOD, up, PERIOD):
        s = struct.pack("<IhhHHHHHH", ts, int(10 * (48 + 3 * math.sin(ts / 400))), int(10 * (39 + math.sin(ts / 500))),
                        int(10 * (8 + 6 * abs(math.sin(ts / 30)))), int(10 * (12 + 8 * abs(math.sin(ts / 40)))),
                        40 + int(10 * math.sin(ts / 200)), 3 + int(2 * math.sin(ts / 150)), 300, 25)
        tin = sum(rates(ts, i)[0] for i in range(6)) / 7200
        tout = sum(rates(ts, i)[1] for i in range(6)) / 7200
        s += struct.pack("<IIII", int(tin * .02), int(tin * .01), int(tout * .015), int(tout * .005))
        for i in range(6):
            rx, tx = rates(ts, i)
            link = LINK[i] if not (i == 3 and ts % 60 < 20) else 3
            s += struct.pack("<IIIIHHBxxx", int(rx / 1000), int(tx / 1000), int(rx / 7200), int(tx / 7200),
                             PERIOD // 5 if i == 2 else 0, 0, link)
        out += s
        n += 1
    size = 36 + 6 * 24
    return b"MCH1" + struct.pack("<BBHHHI", 1, 6, PERIOD, n, size, up) + bytes(out)


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=str(WEB), **k)

    def send_data(self, body, mime):
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/metrics.json":
            return self.send_data(json.dumps(metrics()).encode(), "application/json")
        if path == "/history.bin":
            return self.send_data(history(), "application/octet-stream")
        return super().do_GET()

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print("http://localhost:%d/dashboard.html" % port)
    http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
