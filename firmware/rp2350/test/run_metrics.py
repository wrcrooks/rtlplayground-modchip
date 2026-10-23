#!/usr/bin/env python3
"""Runs test_metrics and checks the /metrics.json it wrote against what the
dashboard reads from it.

  python3 run_metrics.py build/test_metrics build
"""
import json
import pathlib
import subprocess
import sys

failures = 0


def check(cond, name):
    global failures
    print(("PASS  " if cond else "FAIL  ") + name)
    failures += not cond


def main():
    exe, out = sys.argv[1], pathlib.Path(sys.argv[2])
    r = subprocess.run([exe, str(out)], capture_output=True, text=True)
    print(r.stdout, end="")
    if r.returncode:
        return 1
    try:
        m = json.loads((out / "metrics.json").read_text())
    except ValueError as e:
        check(False, "metrics.json parses: %s" % e)
        return 1
    check(True, "metrics.json parses")
    for k in ("t", "uptime", "period", "bridge_up", "ports", "switch", "stp", "lacp", "igmp", "sfp", "modchip",
              "bridge", "events"):
        if k not in m:
            check(False, "metrics.json has %s" % k)
    p = m["ports"]
    check(len(p) == 6 and all(len(x["c"]) == 12 for x in p), "six ports with twelve counters each")
    check(p[0]["name"] == 'uplink "core"', "port names are escaped")
    check(p[0]["mbps"] == 1000 and p[3]["link"] == 0 and p[5]["sfp"], "link speed, a down port, an SFP port")
    check(abs(m["switch"]["temp"] - 45.5) < 0.01 and m["switch"]["mac"] == 17, "switch temperature, MAC entries")
    check(m["stp"] == {"on": True, "root_port": 255, "tc": 2, "root_changes": 0}, "STP")
    check(m["igmp"]["groups"] == 3 and m["igmp"]["joins"] == 9, "IGMP")
    s = m["sfp"][0]
    check(s["present"] and s["ddm"] and abs(s["temp"] - 35.5) < 0.01 and abs(s["vcc"] - 3.3) < 0.001
          and abs(s["bias"] - 6.2) < 0.01 and abs(s["tx_uw"] - 501.2) < 0.05, "SFP diagnostics in natural units")
    check(not m["sfp"][1]["present"], "an empty slot")
    check(m["modchip"]["load"] == 12.5 and m["modchip"]["temp"] == 38.2, "mod chip load and temperature")
    check(m["bridge"]["link_load"] == 10.0 and m["bridge"]["frames_ps"] == 20, "bridge link load, frames/s")
    check(m["events"] == [{"seq": 1, "t": 4000, "port": 4, "link": 0, "mbps": 0}], "link events")
    print("FAILED" if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
