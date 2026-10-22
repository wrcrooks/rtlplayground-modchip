#!/usr/bin/env python3
"""Generate modchip.kicad_sch, the mod chip's schematic, from the tables below.

Every pin of every part is either given a net here or marked not connected,
and the script refuses to write a schematic otherwise. Nets are drawn as
global labels on short wire stubs at the pins, so the net names are the ones
the PCB uses (SW_SCL, SW_U0TXD, ...). The symbols come from KiCad's standard
library and are embedded flattened, as KiCad does.

Run it with KiCad 10's Python so it finds the libraries, then let kicad-cli
bring the file to KiCad 10's format, check it and compare the netlist:

  flatpak run --command=python3 org.kicad.KiCad make_sch.py
  flatpak run --command=kicad-cli org.kicad.KiCad sch upgrade modchip.kicad_sch
  python3 check_sch.py      # ERC and netlist against expected-nets.json

The part choices follow modchip/ARCHITECTURE.md and the RP2350 datasheet
(section 6.3, core voltage regulator; the Pico 2 is the reference design).
"""
import json, os, pathlib, sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import kisexpr as k  # noqa: E402
from kisexpr import Q  # noqa: E402

OUT = HERE / "modchip.kicad_sch"
PROJECT = "modchip"
G = 2.54                         # grid; every position is a multiple of it
STUB = 2.54                      # wire from a pin to its label


def lib_dir():
    for d in (os.environ.get("KICAD10_SYMBOL_DIR"), "/app/extensions/Library/symbols",
              "/usr/share/kicad/symbols"):
        if d and pathlib.Path(d, "Device.kicad_sym").is_file():
            return pathlib.Path(d)
    sys.exit("KiCad symbol library not found; set KICAD10_SYMBOL_DIR")


R0402, C0402, C0603 = "Resistor_SMD:R_0402_1005Metric", "Capacitor_SMD:C_0402_1005Metric", "Capacitor_SMD:C_0603_1608Metric"
SHEET = k.uid("sheet/root")


def font(size=1.27, justify=None, hide=False):
    e = ["effects", ["font", ["size", size, size]]]
    if justify:
        e.append(["justify"] + justify.split())
    if hide:
        e.append(["hide", "yes"])
    return e


def num(v):
    v = round(v, 4)
    return int(v) if v == int(v) else v


class Schematic:
    DIRS = {0: (1, 0), 90: (0, -1), 180: (-1, 0), 270: (0, 1)}

    def __init__(self):
        self.libdir = lib_dir()
        self.libs = {}
        self.lib_symbols = {}
        self.items = []
        self.expected = {}           # "REF.pin" -> net, or None for not connected
        self.points = {}             # label/no-connect position -> net, to catch collisions

    def symbol_def(self, lib_id):
        if lib_id not in self.lib_symbols:
            lib, name = lib_id.split(":")
            if lib not in self.libs:
                self.libs[lib] = k.parse((self.libdir / (lib + ".kicad_sym")).read_text())
            sym = k.library_symbol(self.libs[lib], name)
            sym[1] = Q(lib_id)
            self.lib_symbols[lib_id] = sym
        return self.lib_symbols[lib_id]

    def claim(self, p, net):
        key = (round(p[0], 3), round(p[1], 3))
        if key in self.points and self.points[key] != net:
            sys.exit("two nets meet at %s: %s and %s" % (key, self.points[key], net))
        self.points[key] = net

    def label(self, net, pin_xy, angle):
        dx, dy = self.DIRS[int(angle) % 360]
        end = (pin_xy[0] + dx * STUB, pin_xy[1] + dy * STUB)
        self.claim(pin_xy, net)
        self.claim(end, net)
        self.items.append(["wire", ["pts", ["xy", num(pin_xy[0]), num(pin_xy[1])], ["xy", num(end[0]), num(end[1])]],
                           ["stroke", ["width", 0], ["type", "default"]], ["uuid", Q(k.uid("wire/%s/%s" % (net, pin_xy)))]])
        just = "right" if angle in (180, 270) else "left"
        self.items.append(["global_label", Q(net), ["shape", "passive"], ["at", num(end[0]), num(end[1]), int(angle)],
                           ["fields_autoplaced", "yes"], font(1.27, just), ["uuid", Q(k.uid("label/%s/%s" % (net, pin_xy)))],
                           ["property", Q("Intersheetrefs"), Q("${INTERSHEET_REFS}"), ["at", num(end[0]), num(end[1]), 0],
                            font(1.27, just, hide=True)]])

    def no_connect(self, xy):
        self.claim(xy, None)
        self.items.append(["no_connect", ["at", num(xy[0]), num(xy[1])], ["uuid", Q(k.uid("nc/%s" % (xy,)))]])

    def part(self, ref, lib_id, value, at, nets, footprint=None, dnp=False, fields=None, text_at=None):
        """nets: {pin number or pin name: net, or None for not connected}.
        text_at: (dx, dy) in grid units for the reference, the value goes below it."""
        sym = self.symbol_def(lib_id)
        x, y = at[0] * G, at[1] * G
        pins = k.pins(sym)
        by_pos = {}
        for number, name, ptype, px, py, ang in pins:
            if number in nets:
                net = nets[number]
            elif name in nets:
                net = nets[name]
            else:
                sys.exit("%s pin %s (%s) has no net" % (ref, number, name))
            self.expected["%s.%s" % (ref, number)] = net
            pos = (x + px, y - py)
            if pos in by_pos and by_pos[pos][0] != net:
                sys.exit("%s: stacked pins at %s on different nets" % (ref, pos))
            by_pos[pos] = (net, (ang + 180) % 360)
        for pos, (net, away) in by_pos.items():
            if net is None:
                self.no_connect(pos)
            else:
                self.label(net, pos, away)

        props = {p[1]: p for p in k.find(sym, "property")}
        inst = ["symbol", ["lib_id", Q(lib_id)], ["at", num(x), num(y), 0], ["unit", 1],
                ["exclude_from_sim", "no"], ["in_bom", "yes"], ["on_board", "yes"], ["dnp", "yes" if dnp else "no"],
                ["uuid", Q(k.uid("sym/" + ref))]]
        values = {"Reference": ref, "Value": value}
        if footprint:
            values["Footprint"] = footprint
        for name in ("Reference", "Value", "Footprint", "Datasheet", "Description"):
            src = props.get(name)
            at_l = k.first(src, "at") if src else ["at", 0, 0, 0]
            text = values.get(name, src[2] if src else "")
            hidden = name not in ("Reference", "Value") or (name == "Reference" and ref.startswith("#"))
            px, py = float(at_l[1]), float(at_l[2])
            rot = int(float(at_l[3])) if len(at_l) > 3 else 0
            just = None
            if text_at and name in ("Reference", "Value"):
                px, py, rot, just = text_at[0] * G, -(text_at[1] + (name == "Value")) * G, 0, "left"
            inst.append(["property", Q(name), Q(text), ["at", num(x + px), num(y - py), rot], font(1.27, just, hide=hidden)])
        for name, text in (fields or {}).items():
            inst.append(["property", Q(name), Q(text), ["at", num(x), num(y), 0], font(1.27, hide=True)])
        for number in sorted({p[0] for p in pins}, key=str):
            inst.append(["pin", Q(number), ["uuid", Q(k.uid("pin/%s/%s" % (ref, number)))]])
        inst.append(["instances", ["project", Q(PROJECT), ["path", Q("/" + SHEET), ["reference", Q(ref)], ["unit", 1]]]])
        self.items.append(inst)

    def flag(self, n, net, at):
        self.part("#FLG%02d" % n, "power:PWR_FLAG", "PWR_FLAG", at, {"1": net})
        del self.expected["#FLG%02d.1" % n]

    def text(self, s, at, size=1.27):
        self.items.append(["text", Q(s), ["exclude_from_sim", "no"], ["at", num(at[0] * G), num(at[1] * G), 0],
                           font(size, "left top"), ["uuid", Q(k.uid("text/" + s[:40]))]])

    def write(self, path):
        root = ["kicad_sch", ["version", 20250114], ["generator", Q("eeschema")], ["generator_version", Q("9.0")],
                ["uuid", Q(SHEET)], ["paper", Q("A2")],
                ["title_block", ["title", Q("RTLPlayground mod chip (RP2350)")], ["rev", Q("A0")],
                 ["company", Q("github.com/wrcrooks/RTLPlayground, modchip/")],
                 ["comment", 1, Q("For SWTG024AS-V2.0 switches: management processor on headers T3, T8 and T5")],
                 ["comment", 2, Q("Generated by modchip/hardware/make_sch.py; not built or tested yet")]],
                ["lib_symbols"] + list(self.lib_symbols.values())]
        root += self.items
        root += [["sheet_instances", ["path", Q("/"), ["page", Q("1")]]], ["embedded_fonts", "no"]]
        path.write_text(k.dump(root) + "\n")


def build():
    s = Schematic()
    R = "Device:R"
    C = "Device:C"

    # ---- Power: USB, 3.3 V from USB, ideal-diode OR with the switch's 3.3 V
    ox, oy = 10, 18
    s.text("POWER\nThe switch's 3.3 V (T3/T8 pin 4) and USB's 3.3 V are ORed by two ideal diodes,\n"
           "so neither back-feeds the other. SW_SENSE tells the firmware whether the switch is on:\n"
           "until it is, keep every switch-facing pin high-impedance with its input buffer off.",
           (ox, oy - 8))
    s.part("J4", "Connector:USB_C_Receptacle_USB2.0_16P", "USB-C (bench use)", (ox + 6, oy + 14),
           {"VBUS": "VBUS", "GND": "GND", "SHIELD": "GND", "CC1": "USB_CC1", "CC2": "USB_CC2",
            "D+": "USB_DP_C", "D-": "USB_DM_C", "SBU1": None, "SBU2": None},
           footprint="Connector_USB:USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal")
    s.part("U6", "Power_Protection:USBLC6-2SC6", "USBLC6-2SC6", (ox + 30, oy + 8),
           {"1": "USB_DP_C", "6": "USB_DP_C", "3": "USB_DM_C", "4": "USB_DM_C", "VBUS": "VBUS", "GND": "GND"},
           text_at=(3, -4))
    s.part("R1", R, "5.1k", (ox + 40, oy + 8), {"1": "USB_CC1", "2": "GND"}, R0402)
    s.part("R2", R, "5.1k", (ox + 43, oy + 8), {"1": "USB_CC2", "2": "GND"}, R0402)
    s.part("R3", R, "27", (ox + 46, oy + 8), {"1": "USB_DP_C", "2": "USB_DP"}, R0402)
    s.part("R4", R, "27", (ox + 49, oy + 8), {"1": "USB_DM_C", "2": "USB_DM"}, R0402)
    s.part("U4", "Regulator_Linear:AP2112K-3.3", "AP2112K-3.3", (ox + 30, oy + 30),
           {"VIN": "VBUS", "EN": "VBUS", "GND": "GND", "VOUT": "+3V3_USB", "NC": None}, text_at=(-3, -4))
    s.part("C20", C, "1u", (ox + 40, oy + 30), {"1": "VBUS", "2": "GND"}, C0402)
    s.part("C21", C, "1u", (ox + 43, oy + 30), {"1": "+3V3_USB", "2": "GND"}, C0402)
    for ref, src, row in (("U2", "+3V3_SW", 8), ("U3", "+3V3_USB", 30)):
        s.part(ref, "Power_Management:LM66100DCK", "LM66100", (ox + 62, oy + row),
               {"VIN": src, "VOUT": "+3V3", "~{CE}": "GND", "GND": "GND", "ST": None, "NC": None}, text_at=(4, 3))
    s.part("C24", C, "1u", (ox + 74, oy + 8), {"1": "+3V3_SW", "2": "GND"}, C0402)
    s.part("C22", C, "10u", (ox + 77, oy + 8), {"1": "+3V3", "2": "GND"}, C0603)
    s.part("R22", R, "100k", (ox + 80, oy + 8), {"1": "+3V3_SW", "2": "SW_SENSE"}, R0402)
    s.part("R23", R, "100k", (ox + 83, oy + 8), {"1": "SW_SENSE", "2": "GND"}, R0402)
    for i, net in enumerate(("GND", "VBUS", "+3V3_SW", "+1V1", "VREG_AVDD")):
        s.flag(i + 1, net, (ox + 74 + 5 * i, oy + 30))

    # ---- Core regulator and decoupling (RP2350 datasheet figure 22)
    ox, oy = 10, 64
    s.text("CORE REGULATOR AND DECOUPLING\n"
           "L1, C13 (CIN), C14 (COUT), R5/C16 (VREG_AVDD filter): lay out as the Pico 2 does (datasheet 6.3.8).\n"
           "C15 is the second 4.7u at DVDD pin 23. One 100n per IOVDD and DVDD pin, at the pin.",
           (ox, oy - 8))
    caps = [("C%d" % (i + 1), "100n", "+3V3") for i in range(6)]
    caps += [("C7", "100n", "+3V3"), ("C8", "100n", "+3V3"), ("C9", "100n", "+3V3")]
    caps += [("C10", "100n", "+1V1"), ("C11", "100n", "+1V1"), ("C12", "100n", "+1V1")]
    caps += [("C13", "4.7u", "+3V3"), ("C14", "4.7u", "+1V1"), ("C15", "4.7u", "+1V1"), ("C16", "4.7u", "VREG_AVDD")]
    for i, (ref, val, net) in enumerate(caps):
        s.part(ref, C, val, (ox + 3 * i, oy + 8), {"1": net, "2": "GND"}, C0402)
    s.part("L1", "Device:L", "3.3uH", (ox + 3 * len(caps) + 2, oy + 8), {"1": "VREG_LX", "2": "+1V1"},
           "Inductor_SMD:L_Murata_DFE201610P",
           {"MPN": "Abracon AOTA-B201610S3R3-101-T (check its land pattern against this footprint)"})
    s.part("R5", R, "33", (ox + 3 * len(caps) + 5, oy + 8), {"1": "+3V3", "2": "VREG_AVDD"}, R0402)

    # ---- MCU
    s.text("MCU\nRP2354A: RP2350A with 2 MiB of flash in the package (same pinout).\n"
           "With an RP2350A, fit U5 and C19. Never fit U5 with an RP2354A: both would answer on QSPI_SS.",
           (106, 28))
    s.part("U1", "MCU_RaspberryPi:RP2354A", "RP2354A", (130, 68), {
        "IOVDD": "+3V3", "QSPI_IOVDD": "+3V3", "USB_OTP_VDD": "+3V3", "ADC_AVDD": "+3V3",
        "VREG_VIN": "+3V3", "VREG_AVDD": "VREG_AVDD", "VREG_LX": "VREG_LX", "VREG_FB": "+1V1",
        "DVDD": "+1V1", "VREG_PGND": "GND", "GND": "GND",
        "RUN": "RUN", "USB_DP": "USB_DP", "USB_DM": "USB_DM", "XIN": "XIN", "XOUT": "XOUT",
        "SWCLK": "SWCLK", "SWDIO": "SWDIO",
        "~{QSPI_SS}": "QSPI_SS", "QSPI_SCLK": "QSPI_SCLK", "QSPI_SD0": "QSPI_SD0", "QSPI_SD1": "QSPI_SD1",
        "QSPI_SD2": "QSPI_SD2", "QSPI_SD3": "QSPI_SD3",
        "GPIO0": "UART_TX", "GPIO1": "UART_RX", "GPIO4": "I2C_SDA", "GPIO5": "I2C_SCL",
        "GPIO6": "T3_SPI_DO", "GPIO7": "T3_SPI_CS",
        "GPIO8": "T8_GPIO46", "GPIO9": "T8_GPIO47", "GPIO10": "T8_GPIO48", "GPIO11": "T8_GPIO49",
        "GPIO12": "RESET_DRV", "GPIO25": "LED_GP25", "GPIO26/ADC0": "SW_SENSE",
        **{"GPIO%d" % n: None for n in (2, 3, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24)},
        "GPIO27/ADC1": None, "GPIO28/ADC2": None, "GPIO29/ADC3": None,
    })

    # ---- Flash (RP2350A only), crystal, buttons, debug, LED
    ox, oy = 58, 98
    s.text("FLASH, CLOCK, BOOT, DEBUG\nU5/C19: DNP with the RP2354A. Keep the QSPI stubs to U5 short.",
           (ox, oy - 8))
    s.part("U5", "Memory_Flash:W25Q16JVSS", "W25Q16JVUXIQ", (ox + 10, oy + 8), {
        "~{CS}": "QSPI_SS", "CLK": "QSPI_SCLK", "DI/IO_{0}": "QSPI_SD0", "DO/IO_{1}": "QSPI_SD1",
        "~{WP}/IO_{2}": "QSPI_SD2", "~{HOLD}/~{RESET}/IO_{3}": "QSPI_SD3", "VCC": "+3V3", "GND": "GND"},
        "Package_SON:Winbond_USON-8-1EP_3x2mm_P0.5mm_EP0.2x1.6mm", dnp=True,
        fields={"Fit": "RP2350A builds only; DNP with RP2354A"})
    s.part("C19", C, "100n", (ox + 18, oy + 8), {"1": "+3V3", "2": "GND"}, C0402, dnp=True,
           fields={"Fit": "RP2350A builds only; DNP with RP2354A"})
    s.part("Y1", "Device:Crystal_GND24", "12MHz", (ox + 10, oy + 30),
           {"1": "XIN", "3": "XTAL_OUT", "2": "GND", "4": "GND"},
           "Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm", fields={"MPN": "Abracon ABM8-272-T3"})
    s.part("C17", C, "15p", (ox + 19, oy + 30), {"1": "XIN", "2": "GND"}, C0402)
    s.part("C18", C, "15p", (ox + 22, oy + 30), {"1": "XTAL_OUT", "2": "GND"}, C0402)
    s.part("R6", R, "1k", (ox + 25, oy + 30), {"1": "XOUT", "2": "XTAL_OUT"}, R0402)
    s.part("SW1", "Switch:SW_Push", "BOOTSEL", (ox + 34, oy + 8), {"1": "BOOT_SW", "2": "GND"},
           "Button_Switch_SMD:SW_Push_1P1T_NO_CK_KMR2")
    s.part("R7", R, "1k", (ox + 41, oy + 8), {"1": "QSPI_SS", "2": "BOOT_SW"}, R0402)
    s.part("SW2", "Switch:SW_Push", "RUN", (ox + 34, oy + 30), {"1": "RUN", "2": "GND"},
           "Button_Switch_SMD:SW_Push_1P1T_NO_CK_KMR2")
    s.part("R8", R, "10k", (ox + 41, oy + 30), {"1": "+3V3", "2": "RUN"}, R0402)
    s.part("J5", "Connector_Generic:Conn_01x03", "SWD", (ox + 50, oy + 8),
           {"1": "SWCLK", "2": "GND", "3": "SWDIO"}, "Connector_PinHeader_1.27mm:PinHeader_1x03_P1.27mm_Vertical")
    s.part("D1", "Device:LED", "green", (ox + 50, oy + 30), {"1": "GND", "2": "LED_A"}, "LED_SMD:LED_0402_1005Metric")
    s.part("R9", R, "330", (ox + 56, oy + 30), {"1": "LED_GP25", "2": "LED_A"}, R0402)

    # ---- Switch interfaces
    ox, oy = 170, 18
    s.text("SWITCH HEADERS (J1-J3 are male headers on the board's underside)\n"
           "100R in series on every switch-facing line; I2C pull-ups to the switch's 3.3 V.\n"
           "J3 pin 4 (the switch's 3.3 V on T5) is left open.",
           (ox, oy - 8))
    s.part("J1", "Connector_Generic:Conn_01x06", "To switch T3 (I2C slave)", (ox + 10, oy + 10),
           {"1": "SW_SCL", "2": "GND", "3": "SW_SDA", "4": "+3V3_SW", "5": "SW_SPI_DO", "6": "SW_SPI_CS"},
           "Connector_PinHeader_2.00mm:PinHeader_1x06_P2.00mm_Vertical")
    s.part("J2", "Connector_Generic:Conn_01x06", "To switch T8 (GPIO46-49)", (ox + 10, oy + 30),
           {"1": "SW_GPIO46", "2": "GND", "3": "SW_GPIO48", "4": "+3V3_SW", "5": "SW_GPIO47", "6": "SW_GPIO49"},
           "Connector_PinHeader_2.00mm:PinHeader_1x06_P2.00mm_Vertical")
    s.part("J3", "Connector_Generic:Conn_01x04", "To switch T5 (UART0)", (ox + 10, oy + 48),
           {"1": "SW_U0TXD", "2": "GND", "3": "SW_U0RXD", "4": None},
           "Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical")
    series = [("R10", "UART_TX", "SW_U0RXD"), ("R11", "UART_RX", "SW_U0TXD"),
              ("R12", "I2C_SDA", "SW_SDA"), ("R13", "I2C_SCL", "SW_SCL"),
              ("R14", "T3_SPI_DO", "SW_SPI_DO"), ("R15", "T3_SPI_CS", "SW_SPI_CS"),
              ("R16", "T8_GPIO46", "SW_GPIO46"), ("R17", "T8_GPIO47", "SW_GPIO47"),
              ("R18", "T8_GPIO48", "SW_GPIO48"), ("R19", "T8_GPIO49", "SW_GPIO49")]
    for i, (ref, a, b) in enumerate(series):
        s.part(ref, R, "100", (ox + 22 + 3 * i, oy + 10), {"1": a, "2": b}, R0402)
    s.part("R20", R, "4.7k", (ox + 54, oy + 10), {"1": "+3V3_SW", "2": "SW_SDA"}, R0402)
    s.part("R21", R, "4.7k", (ox + 57, oy + 10), {"1": "+3V3_SW", "2": "SW_SCL"}, R0402)
    s.text("SOC RESET\nTP1 goes to D3's ANODE on the switch: the front pad, by the \"+\".\n"
           "The banded end is the 3.3 V rail. R25 limits the current if the wire is misplaced.",
           (ox + 20, oy + 28))
    s.part("Q1", "Transistor_FET:2N7002", "2N7002", (ox + 26, oy + 44), {"G": "RESET_DRV", "S": "GND", "D": "RESET_D"},
           text_at=(3, 1))
    s.part("R24", R, "100k", (ox + 33, oy + 44), {"1": "RESET_DRV", "2": "GND"}, R0402)
    s.part("R25", R, "1k", (ox + 37, oy + 44), {"1": "RESET_D", "2": "SW_RESET_N"}, R0402)
    s.part("TP1", "Connector:TestPoint", "Reset wire to D3 anode", (ox + 44, oy + 44), {"1": "SW_RESET_N"},
           "TestPoint:TestPoint_THTPad_D1.5mm_Drill0.7mm")

    s.write(OUT)
    (HERE / "expected-nets.json").write_text(json.dumps(s.expected, indent=1, sort_keys=True) + "\n")
    print("wrote %s: %d parts, %d pins" % (OUT.name, len([i for i in s.items if i[0] == "symbol"]), len(s.expected)))


build()
