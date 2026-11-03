#!/usr/bin/env python3
"""Lay out modchip-poe.kicad_pcb from modchip-poe.kicad_sch, inside the outline from outline.py.

The mod chip for the MokerLink POE-2G040210GSM: an 18.9 x 27.5 mm board on T7, beside the right
SFP cage. Uses kicad-agent-kit like ../make_pcb.py; build.sh runs the whole pipeline.

  make_pcb.py            board from the schematic: outline, J1 on T7's holes, hand placement,
                         auto-placement, planes, fanout
  make_pcb.py --pour     after routing: GND pours on the outer layers

4 layers: F.Cu parts and signals / In1.Cu GND plane / In2.Cu +3V3 plane / B.Cu signals; the
bottom side carries only the male header J1. Coordinates are outline.py's (origin T7 pin 1,
y towards the front), shifted by OFFSET onto the sheet.
"""
import pathlib, sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import outline as o  # noqa: E402
from kak import pcb  # noqa: E402
from kak.project import netclass, write_project  # noqa: E402

pcbnew = pcb.pcbnew
SCH = str(HERE / "modchip-poe.kicad_sch")
PCB = str(HERE / "modchip-poe.kicad_pcb")
PRO = str(HERE / "modchip-poe.kicad_pro")
OFFSET = (60.0, 60.0)
THICKNESS = 1.0
POWER_NETS = ("GND", "+3V3", "+1V1", "+3V3_SW", "VREG_AVDD")
VIA = (0.5, 0.2)               # 0.2 mm drill: JLC 4-layer standard
U1_AT = (8.2, -10.8)

# Hand placement: (x, y, rotation) of each footprint's centre, outline.py coordinates.
# U1 at 0: its left side (GP0..GP11) faces J1, GP8..GP11 in the order of T7 pins 6, 5, 3, 1.
PLACEMENT = {
    "U1": (U1_AT[0], U1_AT[1], 0),
    # core regulator above U1's top-right pins (46 AVDD, 47 PGND, 48 LX, 49 VIN, 50 FB):
    # L1 straight above LX, CIN by VIN, the VREG_AVDD RC by pin 46, COUT beyond L1
    "L1": (10.2, -16.6, 90), "C13": (8.6, -16.4, 90), "C16": (11.9, -16.1, 90), "R5": (13.2, -16.1, 90),
    "C14": (10.2, -18.7, 0),
    # flash above U1, turned so its pins 5..8 (SD0, SCLK, SD3, VCC) sit over U1's 57..54; SS, SD1
    # and SD2 go round its left side to the far row
    "U5": (7.2, -19.4, 180), "C19": (4.6, -18.4, 90),
    # crystal in the corner below U1, XTAL_OUT (pad 3) towards XOUT; XIN runs round its left side
    "Y1": (5.6, -2.7, 0), "R6": (7.6, -5.1, 180), "C17": (4.6, -5.1, 0), "C18": (9.0, -3.3, 90),
    # RUN pull-up and the switch-power divider between the crystal and J5
    "R8": (11.0, -5.1, 0), "R22": (11.6, -2.6, 90), "R23": (10.5, -2.6, 90),
    # LED in the right-hand strip, above J5
    "R9": (13.5, -8.75, 90), "D1": (13.5, -11.6, 90),
    # switch side: series resistors between J1 and U1, pull-ups and the ideal diode left of J1
    "R13": (2.3, -1.6, 90), "R12": (2.3, -4.6, 90), "R14": (2.6, -11.0, 90), "R15": (2.6, -13.5, 90),
    "R21": (-2.45, -1.0, 90), "R20": (-2.45, -3.9, 90),
    "C24": (-2.45, -6.6, 0), "U2": (-2.45, -9.0, 90), "C22": (-2.45, -11.6, 0),
    # UART series resistors on the way to J3
    "R11": (-2.0, -17.6, 0), "R10": (2.6, -17.6, 90),
}
J3_PADS = [(-2.6 + 2.54 * i, -24.4) for i in range(4)]
J5_PADS = [(13.6, -1.9 - 1.27 * i) for i in range(5)]
FIXED_EXACT = {"J1": o.T7, "J3": J3_PADS, "J5": J5_PADS}
BOTTOM = {"J1"}


def sw(p):
    return (p[0] + OFFSET[0], p[1] + OFFSET[1])


def rel(dx, dy):
    return (U1_AT[0] + dx, U1_AT[1] + dy)


def project_settings():
    write_project(PRO,
                  [netclass("Default", clearance=0.2, track=0.15, via_d=VIA[0], via_drill=VIA[1]),
                   netclass("Power", clearance=0.2, track=0.3, via_d=VIA[0], via_drill=VIA[1])],
                  [("Power", n) for n in POWER_NETS + ("VREG_LX",)],
                  severities={"lib_footprint_mismatch": "ignore"})


def draw_outline(board, origin):
    pts = o.OUTLINE + o.OUTLINE[:1]
    for a, b in zip(pts, pts[1:]):
        s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_SEGMENT)
        s.SetStart(pcb.vec(*sw(a))); s.SetEnd(pcb.vec(*sw(b)))
        s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(pcb.MM(0.05))
        board.Add(s)


def place_on_holes(fp, targets, bottom):
    """Put fp so that pad n lands on targets[n-1], trying each orientation"""
    if bottom and not fp.IsFlipped():
        fp.Flip(fp.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
    want = [pcb.vec(*sw(t)) for t in targets]
    for deg in (0, 90, 180, 270):
        fp.SetOrientationDegrees(deg)
        pads = {int(p.GetNumber()): p for p in fp.Pads()}
        fp.Move(want[0] - pads[1].GetPosition())
        if all((pads[n + 1].GetPosition() - w).EuclideanNorm() < pcb.MM(0.001) for n, w in enumerate(want)):
            return
    sys.exit("%s: no orientation puts its pads on the targets" % fp.GetReference())


def outside_boxes():
    """Rectangles covering the outline's bounding box minus the outline, for autoplace keep-outs"""
    xs = sorted({p[0] for p in o.OUTLINE}); ys = sorted({p[1] for p in o.OUTLINE})
    boxes = []
    for y0, y1 in zip(ys, ys[1:]):
        for x0, x1 in zip(xs, xs[1:]):
            if not o.inside(((x0 + x1) / 2, (y0 + y1) / 2), o.OUTLINE):
                a, b = sw((x0, y0)), sw((x1, y1))
                boxes.append((a[0], a[1], b[0], b[1]))
    return boxes


def edge_keepouts(margin=0.45):
    boxes = []
    for a, b in o.edges(o.OUTLINE):
        (x0, y0), (x1, y1) = sw(a), sw(b)
        boxes.append((min(x0, x1) - margin, min(y0, y1) - margin, max(x0, x1) + margin, max(y0, y1) + margin))
    return boxes


def header_keepouts():
    """Copper of the THT pads on the top side"""
    out = []
    for h in o.T7 + J3_PADS:
        x, y = sw(h); out.append((x - 1.05, y - 1.05, x + 1.05, y + 1.05))
    for h in J5_PADS:
        x, y = sw(h); out.append((x - 0.65, y - 0.65, x + 0.65, y + 0.65))
    return out


def reference_drawings(board):
    """The switch underneath (Dwgs.User) and notes (Cmts.User), for the reviewer"""
    def text(s, p, layer, size=0.8):
        t = pcbnew.PCB_TEXT(board)
        t.SetText(s); t.SetPosition(pcb.vec(*sw(p))); t.SetLayer(layer)
        t.SetTextSize(pcb.vec(size, size)); t.SetTextThickness(pcb.MM(size * 0.15))
        board.Add(t)

    for label, sh in o.OBSTACLES.items():
        if sh[0] == "rect":
            x0, y0, x1, y1 = sh[1:]
            x0, x1 = max(x0, -12.0), min(x1, 20.0)
            y0, y1 = max(y0, -30.0), min(y1, 8.0)
            s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_RECTANGLE)
            s.SetStart(pcb.vec(*sw((x0, y0)))); s.SetEnd(pcb.vec(*sw((x1, y1))))
            s.SetLayer(pcbnew.Dwgs_User); s.SetWidth(pcb.MM(0.1)); board.Add(s)
            text(label.split(" (")[0].split(",")[0], ((x0 + x1) / 2, (y0 + y1) / 2), pcbnew.Dwgs_User, 0.7)
        else:
            _, cx, cy, rad = sh
            c = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_CIRCLE)
            c.SetCenter(pcb.vec(*sw((cx, cy)))); c.SetEnd(pcb.vec(*sw((cx + rad, cy))))
            c.SetLayer(pcbnew.Dwgs_User); c.SetWidth(pcb.MM(0.1)); board.Add(c)
            text("screw", (cx, cy), pcbnew.Dwgs_User, 0.7)
    for h in o.T9 + o.T10:
        c = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_CIRCLE)
        c.SetCenter(pcb.vec(*sw(h))); c.SetEnd(pcb.vec(*sw((h[0] + 0.5, h[1]))))
        c.SetLayer(pcbnew.Dwgs_User); c.SetWidth(pcb.MM(0.1)); board.Add(c)
    text("T9 (wires to J3)", (o.T9[0][0], o.T9[0][1] - 1.5), pcbnew.Dwgs_User, 0.7)
    notes = [
        "Dwgs.User: the switch underneath (2M-PCB49-V1.1, origin T7 pin 1).",
        "J1: male header on the BOTTOM, into T7. J3: wires, pad n to T9 pin n.",
        "T10 under the board must stay without pins.",
        "T7 pins 1/3 and T9 pins 1/3 are guesses: check them before ordering.",
        "U1 RP2350A + U5 W25Q16JVUXIQ (default); with an RP2354A leave U5 and C19 off.",
    ]
    for i, n in enumerate(notes):
        text(n, (5.0, 4.5 + 1.4 * i), pcbnew.Cmts_User, 0.8)


def check_overlaps(board):
    boxes = [(fp.GetReference(), pcb._abs_box(fp)) for fp in board.GetFootprints()
             if not fp.IsFlipped() and fp.GetReference()]
    bad = []
    for i, (ra, a) in enumerate(boxes):
        for rb, b in boxes[i + 1:]:
            if a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]:
                bad.append((ra, rb))
    print("courtyard overlaps:", bad)
    return bad


def title(board):
    tb = board.GetTitleBlock()
    tb.SetTitle("RTLPlayground mod chip for the POE-2G040210GSM (RP2350)")
    tb.SetCompany("github.com/wrcrooks/rtlplayground-modchip")
    tb.SetRevision("A0")
    tb.SetComment(0, "MokerLink POE-2G040210GSM (2M-PCB49-V1.1): on T7, wires to T9")
    tb.SetComment(1, "Positions measured on a photo (+/-0.5 mm); T7/T9 signal pins are guesses")
    tb.SetComment(2, "Generated by hardware/poe/make_pcb.py + Freerouting; not built or tested yet")


def preroute(board):
    """The regulator's switching node and analogue supply, and the +1V1 ring under U1, locked"""
    r = pcb.Router(board, OFFSET)
    top = U1_AT[1] - 4.15          # just above the top pad row
    # LX narrow between its neighbours, wide once clear of them
    r.route("VREG_LX", [("U1", 48), (10.2, top - 0.35)], width=0.2)
    r.route("VREG_LX", [(10.2, top - 0.35), ("L1", 1)], width=0.4)
    r.route("VREG_AVDD", [("U1", 46), (11.0, top), ("C16", 1)], width=0.2)
    # PGND inward to the exposed pad's corner
    r.route("GND", [("U1", 47), (10.6, U1_AT[1] - 2.5), (9.85, U1_AT[1] - 1.75)], width=0.2)
    # +3V3 pins of the top row: VIN 49 and USB_OTP_VDD 53 on a trunk just above the pads to CIN (C13),
    # whose own via reaches the plane; QSPI_IOVDD 54 straight up to the flash's VCC
    r.route("+3V3", [("U1", 49), (9.8, top), (8.2, top), ("U1", 53)], width=0.2)
    r.route("+3V3", [(8.6, top), ("C13", 1)], width=0.2)
    r.route("+3V3", [("U1", 54), (7.8, top), (7.95, top - 0.5), ("U5", 8)], width=0.2)
    # flash: SD0, SCLK and SD3 straight up to the flash's near row (Freerouting boxes SCLK in otherwise)
    for pin, fpin in ((57, 5), (56, 6), (55, 7)):
        net = board.FindFootprintByReference("U5").FindPadByNumber(str(fpin)).GetNetname()
        r.route(net, [("U1", pin), ("U5", fpin)], width=0.15)
    # T7_P6 (GP8) up a lane left of U1's pads to R15, leaving the space beside the pads for UART
    r.route("T7_P6", [("U1", 12), (3.5, -9.2), (3.25, -9.45), (3.25, -12.99), ("R15", 1)], width=0.15)
    # +1V1: DVDD pins 6 (left), 23 (bottom), 39 (right) and VREG_FB 50 (top) each drop a via between
    # the pad ring and the exposed pad, joined on B.Cu by a cross through the exposed pad's thermal
    # vias (its B.Cu copper is removed in build()). That leaves room beside them for the plane vias
    # of the +3V3 pins 20 and 38, which can't go outwards: XIN and SW_SENSE leave there.
    y = U1_AT[1] - 0.8
    xc = U1_AT[0] + 0.725
    vias = {6: (U1_AT[0] - 2.4, y), 23: rel(0.0, 2.4), 39: (U1_AT[0] + 2.4, y), 50: rel(1.2, -2.4)}
    for pin, v in vias.items():
        # pin 39 inwards first, clear of SW_SENSE's pad below it
        r.route("+1V1", [("U1", pin)] + ([(U1_AT[0] + 2.75, U1_AT[1] - 0.4)] if pin == 39 else []) + [v], width=0.2)
        r.via("+1V1", v, drill=VIA[1], size=VIA[0])
    r.route("+1V1", [vias[6], vias[39]], width=0.3, layer="B.Cu")
    r.route("+1V1", [(xc, y), (xc, U1_AT[1] - 1.95), vias[50]], width=0.3, layer="B.Cu")
    r.route("+1V1", [(xc, y), (xc, U1_AT[1] + 1.8), vias[23]], width=0.3, layer="B.Cu")
    for pin, v in ((20, rel(-1.2, 2.4)), (38, rel(2.4, 0.2))):
        r.route("+3V3", [("U1", pin)] + ([rel(2.75, 0.0)] if pin == 38 else []) + [v], width=0.2)
        r.via("+3V3", v, drill=VIA[1], size=VIA[0])


def planes(board, rings):
    poly = [sw(p) for p in o.OUTLINE]
    board.SetLayerType(board.GetLayerID("In1.Cu"), pcbnew.LT_POWER)
    board.SetLayerType(board.GetLayerID("In2.Cu"), pcbnew.LT_POWER)
    pcb.zone(board, "GND", board.GetLayerID("In1.Cu"), poly)
    pcb.zone(board, "+3V3", board.GetLayerID("In2.Cu"), poly)
    pcb.fill_zones(board)
    u1 = {f"U1.{p.GetNumber()}" for p in board.FindFootprintByReference("U1").Pads()}
    failed = pcb.fanout(board, {"GND", "+3V3"}, via_d=VIA[0], via_drill=VIA[1], clearance=0.206,
                        skip_refs=("J1", "J3", "J5"), prefer_inward=u1, via_keepouts=rings,
                        keepouts=edge_keepouts(0.3), max_reach=2.5, force=("C13.1",))
    print("fanout failed:", failed)
    pcb.fill_zones(board)


def build():
    board = pcb.build_from_schematic(SCH, PCB, dict(PLACEMENT), copper_layers=4,
                                     origin=OFFSET, outline=draw_outline, silk_refs=False,
                                     silk_ref_min=3.0)
    ds = board.GetDesignSettings()
    ds.SetBoardThickness(pcb.MM(THICKNESS))
    ds.SetAuxOrigin(pcb.vec(*sw(o.T7[0])))
    ds.SetGridOrigin(pcb.vec(*sw(o.T7[0])))
    title(board)
    # header outlines on silk would run over the parts beside them; J5 is bare pads (no header is
    # fitted), so its courtyard goes too: header_keepouts() covers the copper
    for ref, layers in (("J3", (pcbnew.F_SilkS,)), ("J5", (pcbnew.F_SilkS, pcbnew.F_CrtYd))):
        fp = board.FindFootprintByReference(ref)
        for g in list(fp.GraphicalItems()):
            if g.GetLayer() in layers:
                fp.Remove(g)
    # U5's LCSC footprint draws its outline and pin-1 mark over the pads: a dot outside pin 1 instead
    u5 = board.FindFootprintByReference("U5")
    for g in list(u5.GraphicalItems()):
        if g.GetLayer() == pcbnew.F_SilkS:
            u5.Remove(g)
    dot = pcbnew.PCB_SHAPE(u5, pcbnew.SHAPE_T_CIRCLE)
    p1 = u5.FindPadByNumber("1").GetPosition()
    dot.SetCenter(p1 + pcb.vec(0.6, -0.55)); dot.SetEnd(p1 + pcb.vec(0.68, -0.55))
    dot.SetLayer(pcbnew.F_SilkS); dot.SetWidth(pcb.MM(0.15)); dot.SetFilled(True)
    u5.Add(dot)
    for ref, holes in FIXED_EXACT.items():
        place_on_holes(board.FindFootprintByReference(ref), holes, bottom=ref in BOTTOM)
    u1 = board.FindFootprintByReference("U1")
    for p in u1.Pads():                # the exposed pad's B.Cu copper (the +1V1 cross runs there)
        if list(p.GetLayerSet().Seq()) == [pcbnew.B_Cu]:   # becomes a copy of the F.Cu one
            ls = pcbnew.LSET(); ls.AddLayer(pcbnew.F_Cu); p.SetLayerSet(ls)
    rings = pcb.escape_rings(board, ["U1"], width=0.8)
    x0, y0 = sw((min(x for x, _ in o.OUTLINE), min(y for _, y in o.OUTLINE)))
    x1, y1 = sw((max(x for x, _ in o.OUTLINE), max(y for _, y in o.OUTLINE)))
    # corridors auto-placed parts leave clear: the regulator corner, the flash's lanes from U1 and round it
    corridors = [(8.0, -19.2, 13.6, -15.0), (4.9, -23.6, 8.6, -21.2), (5.1, -21.2, 6.3, -14.9),
                 (6.3, -17.6, 8.4, -14.9)]
    keep = [sw(c[:2]) + sw(c[2:]) for c in corridors]
    failed = pcb.autoplace(board, list(PLACEMENT) + list(FIXED_EXACT), power_nets=POWER_NETS,
                           area=(x0, y0, x1, y1), gap=0.2,
                           keepouts=outside_boxes() + edge_keepouts() + header_keepouts() + rings + keep)
    print("autoplace failed:", failed)
    if failed and "--place-only" not in sys.argv:
        sys.exit("autoplace could not place %s" % failed)
    if check_overlaps(board) and "--place-only" not in sys.argv:
        sys.exit("fix the placement")
    reference_drawings(board)
    return board, rings


def main():
    if "--pour" in sys.argv:
        board = pcbnew.LoadBoard(PCB)
        for z in list(board.Zones()):
            if z.GetLayer() in (pcbnew.F_Cu, pcbnew.B_Cu) and not z.GetIsRuleArea():
                board.Delete(z)
        for t in list(board.GetTracks()):      # Freerouting leaves a few zero-length stubs
            if t.GetClass() == "PCB_TRACK" and t.GetLength() < pcb.MM(0.01):
                board.Delete(t)
        poly = [sw(p) for p in o.OUTLINE]
        pcb.zone(board, "GND", pcbnew.F_Cu, poly)
        pcb.zone(board, "GND", pcbnew.B_Cu, poly)
        pcb.fill_zones(board)
        for ref in ("J1", "J3", "J5"):
            board.FindFootprintByReference(ref).SetExcludedFromPosFiles(True)
        comps, _ = pcb.export_netlist(SCH)
        for fp in board.GetFootprints():
            c = comps.get(fp.GetReference())
            if c and c["fields"].get("Description"):
                fp.GetField(pcbnew.FIELD_T_DESCRIPTION).SetText(c["fields"]["Description"])
    else:
        board, rings = build()
        if "--place-only" not in sys.argv:
            preroute(board)
            planes(board, rings)
    pcbnew.SaveBoard(PCB, board)
    project_settings()
    print("saved", PCB)


if __name__ == "__main__":
    main()
