#!/usr/bin/env python3
"""Check modchip.kicad_sch with KiCad 10: run ERC, export the netlist and
compare every pin's net with expected-nets.json, which make_sch.py writes.

  python3 check_sch.py

ERC must come back clean.
"""
import json, pathlib, subprocess, sys, tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import kisexpr as k  # noqa: E402

KICAD_CLI = ["flatpak", "run", "--command=kicad-cli", "org.kicad.KiCad"]
SCH = HERE / "modchip.kicad_sch"
ALLOWED = []


def cli(*args):
    subprocess.run(KICAD_CLI + list(args), check=True, capture_output=True, timeout=300)


def main():
    ok = True
    with tempfile.TemporaryDirectory(dir=HERE) as tmp:
        erc, net = pathlib.Path(tmp, "erc.json"), pathlib.Path(tmp, "net.kicad_net")
        cli("sch", "erc", "--format", "json", "--severity-all", "-o", str(erc), str(SCH))
        cli("sch", "export", "netlist", "--format", "kicadsexpr", "-o", str(net), str(SCH))
        report = json.loads(erc.read_text())
        netlist = k.parse(net.read_text())

    for v in (v for s in report["sheets"] for v in s["violations"]):
        refs = {d["description"].split()[1] for d in v["items"] if d["description"].startswith("Symbol ")}
        if any(v["type"] == t and refs == r for t, r in ALLOWED):
            print("ERC (expected)  %s: %s" % (v["type"], v["description"]))
            continue
        ok = False
        print("ERC %s  %s: %s %s" % (v["severity"], v["type"], v["description"],
                                     [d["description"] for d in v["items"]]))

    actual = {}
    for n in k.find(k.first(netlist, "nets"), "net"):
        name = k.first(n, "name")[1]
        for node in k.find(n, "node"):
            actual["%s.%s" % (k.first(node, "ref")[1], k.first(node, "pin")[1])] = name
    expected = json.loads((HERE / "expected-nets.json").read_text())
    bad = 0
    for pin, want in sorted(expected.items()):
        got = actual.get(pin)
        if want is None:
            # Not connected: KiCad puts the pin alone on an unnamed net
            if got is not None and not got.startswith("unconnected-"):
                print("pin %-8s should be open, is on %s" % (pin, got))
                bad += 1
        elif got != want:
            print("pin %-8s on %s, expected %s" % (pin, got, want))
            bad += 1
    extra = sorted(set(actual) - set(expected))
    for pin in extra:
        if not pin.startswith("#"):
            print("pin %-8s on %s is not in expected-nets.json" % (pin, actual[pin]))
            bad += 1
    nets = {v for v in actual.values() if not v.startswith("unconnected-")}
    print("netlist: %d pins checked, %d nets, %d mismatches" % (len(expected), len(nets), bad))
    sys.exit(0 if ok and not bad else 1)


main()
