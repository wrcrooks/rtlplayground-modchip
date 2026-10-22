#!/usr/bin/env python3
"""Lay out modchip.kicad_pcb from modchip.kicad_sch, inside the outline from outline.py.

Uses kicad-agent-kit (a local checkout; build.sh finds it through $KAK) for
the board-from-netlist build, auto-placement, escape rings, planes and
fanout. build.sh runs the whole pipeline, Freerouting included:

  KAK=~/Desktop/Workspace/KiCad/kicad-agent-kit ./build.sh

Steps here (run in KiCad 10's Python via kak.sh py):
  make_pcb.py            board from the schematic: outline, headers on the switch's
                         holes, hand placement, auto-placement, planes, fanout
  make_pcb.py --pour     after routing: GND pours on the outer layers

4 layers: F.Cu parts and signals / In1.Cu GND plane / In2.Cu +3V3 plane /
B.Cu signals; the bottom side carries only the male headers J1-J3.
Coordinates are switch-board millimetres (x from the switch's left edge, y
from its rear edge), shifted by OFFSET onto the sheet.
"""
import pathlib, sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import outline as o  # noqa: E402
from kak import pcb  # noqa: E402
from kak.project import netclass, write_project  # noqa: E402

pcbnew = pcb.pcbnew
SCH = str(HERE / "modchip.kicad_sch")
PCB = str(HERE / "modchip.kicad_pcb")
PRO = str(HERE / "modchip.kicad_pro")
OFFSET = (60.0, 40.0)
THICKNESS = 1.0                  # the stack in ARCHITECTURE.md assumes 1.0 mm
POWER_NETS = ("GND", "+3V3", "+1V1", "+3V3_SW", "VREG_AVDD")
VIA = (0.5, 0.2)               # 0.2 mm drill: JLC 4-layer standard; keeps 0.35 mm track-to-hole

# Hand placement: (x, y, rotation) of each footprint's origin, switch coordinates.
# U1 at 270: its GP11..GP0 then face T3/T8 in header order (see make_sch.py).
PLACEMENT = {
    "U1": (39.0, 10.6, 270),
    # core regulator, right of U1's VREG pins (46 AVDD, 47 PGND, 48 LX, 49 VIN, 50 FB):
    # L1 in line with LX, COUT after it, CIN above, the VREG_AVDD RC below
    "L1": (46.3, 13.0, 0), "C14": (48.2, 12.52, 90), "C13": (45.2, 11.1, 0),
    "C16": (44.4, 14.7, 270), "R5": (46.6, 15.5, 90),
    # +1V1 caps beside L1 and COUT; +1V1 is routed (B.Cu under U1 is nearly free)
    "C15": (50.2, 12.9, 90), "C10": (49.2, 14.6, 0), "C11": (51.2, 14.6, 0), "C12": (50.2, 16.0, 0),
    # clock, debug and LED on the left
    "Y1": (29.6, 6.9, 0),                         # above the SWD lines: XIN/XOUT leave U1 above them
    "J5": (24.3, 9.6, 0),                         # SWD pads, 1.27 mm: 3V3, SWCLK, GND, SWDIO, RUN
    "D1": (28.6, 16.4, 0),
    # flash: 208-mil SOIC-8 W25Q128JVSIQ (left off in an RP2354A build), right of U1's QSPI pins
    "U5": (53.6, 8.8, 180),                       # SD0/CLK/SD3 face U1 in its pin order
    # switch lines: 100 R under each header pin they serve, lying flat so that a routing
    # channel stays open between them and U1's top pins
    "R13": (32.4, 4.0, 0), "R12": (36.4, 4.0, 0), "R14": (40.4, 4.0, 0), "R15": (42.4, 4.0, 0),
    "R16": (46.4, 4.0, 0), "R18": (50.4, 4.0, 0), "R17": (54.4, 4.0, 0), "R19": (56.4, 4.0, 0),
    "U2": (59.6, 5.4, 90),                        # switch 3.3 V into the ideal-diode OR
    "Q1": (55.6, 14.3, 0),                        # reset driver, left of TP1
    # UART series resistors beside J3 in the tail
    "R11": (121.5, 29.2, 0), "R10": (121.5, 31.9, 0),
}
FIXED_EXACT = {"J1": o.T3, "J2": o.T8, "J3": o.T5, "TP1": [o.RESET_PAD]}


def sw(p):
    return (p[0] + OFFSET[0], p[1] + OFFSET[1])


def project_settings():
    write_project(PRO,
                  [netclass("Default", clearance=0.2, track=0.15, via_d=VIA[0], via_drill=VIA[1]),   # 0.4 mm-pitch QFN: 0.15 leaves 0.225 to the next pad
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
    sys.exit("%s: no orientation puts its pads on the holes" % fp.GetReference())


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
    """Thin boxes along every outline edge: parts keep this far from the board edge"""
    boxes = []
    for a, b in o.edges(o.OUTLINE):
        (x0, y0), (x1, y1) = sw(a), sw(b)
        boxes.append((min(x0, x1) - margin, min(y0, y1) - margin, max(x0, x1) + margin, max(y0, y1) + margin))
    return boxes


def header_keepouts():
    """Top-side copper of the THT header pads and TP1"""
    out = []
    for h in o.T3 + o.T8:
        x, y = sw(h); out.append((x - 1.0, y - 1.0, x + 1.0, y + 1.0))
    for h in o.T5:
        x, y = sw(h); out.append((x - 1.2, y - 1.2, x + 1.2, y + 1.2))
    x, y = sw(o.RESET_PAD); out.append((x - 1.1, y - 1.1, x + 1.1, y + 1.1))
    return out


def reference_drawings(board):
    """The switch underneath (Dwgs.User) and U8's clip zone (Cmts.User), for the reviewer"""
    def rect(layer, a, b, w=0.1):
        s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_RECTANGLE)
        s.SetStart(pcb.vec(*sw(a))); s.SetEnd(pcb.vec(*sw(b))); s.SetLayer(layer); s.SetWidth(pcb.MM(w))
        board.Add(s)

    def text(s, p, layer, size=0.8):
        t = pcbnew.PCB_TEXT(board)
        t.SetText(s); t.SetPosition(pcb.vec(*sw(p))); t.SetLayer(layer)
        t.SetTextSize(pcb.vec(size, size)); t.SetTextThickness(pcb.MM(size * 0.15))
        board.Add(t)

    rect(pcbnew.Dwgs_User, (0, 0), o.BOARD, 0.15)
    text("SWTG024AS-V2.0 PCB edge (ports at the bottom)", (o.BOARD[0] / 2, o.BOARD[1] - 2), pcbnew.Dwgs_User, 1.5)
    for label, sh in o.OBSTACLES.items():
        if sh[0] == "rect":
            x0, y0, x1, y1 = sh[1:]
            rect(pcbnew.Dwgs_User, (x0, max(y0, 0)), (x1, min(y1, o.BOARD[1])))
            where = ((x0 + x1) / 2, (max(y0, 0) + min(y1, o.BOARD[1])) / 2)
        else:
            _, cx, cy, rad = sh
            c = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_CIRCLE)
            c.SetCenter(pcb.vec(*sw((cx, cy)))); c.SetEnd(pcb.vec(*sw((cx + rad, cy))))
            c.SetLayer(pcbnew.Dwgs_User); c.SetWidth(pcb.MM(0.1)); board.Add(c)
            where = (cx, cy)
        text(label.split(" (")[0], where, pcbnew.Dwgs_User)
    for label, sh in o.KEEPOUTS.items():
        rect(pcbnew.Cmts_User, sh[1:3], sh[3:5], 0.15)
        text(label, ((sh[1] + sh[3]) / 2, sh[4] - 1.5), pcbnew.Cmts_User)
    notes = [
        "Dwgs.User: the switch underneath. Cmts.User: keep U8 reachable for a clip.",
        "J1-J3 are male headers on the BOTTOM side; the switch gets low-profile sockets (about 4.5 mm).",
        "Top-side parts: 3.2 mm at most, for about 8.7 mm above the switch PCB in all.",
        "TP1: wire to D3's ANODE (front pad, by the +), never the banded end (3.3 V rail).",
        "U1 RP2350A + U5 W25Q128JVSIQ (default); for an RP2354A build leave U5 and C19 off.",
        "Header holes measured on a photo (+/-0.3 mm): check them with calipers before ordering.",
    ]
    for i, n in enumerate(notes):
        text(n, (o.BOARD[0] / 2, o.BOARD[1] + 6 + 2.2 * i), pcbnew.Cmts_User, 1.2)


def check_overlaps(board):
    """Courtyard boxes of top-side parts must not overlap (hand placements skip autoplace's check)"""
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
    tb.SetTitle("RTLPlayground mod chip (RP2350)")
    tb.SetCompany("github.com/wrcrooks/RTLPlayground, modchip/")
    tb.SetRevision("A0")
    tb.SetComment(0, "For SWTG024AS-V2.0 (MokerLink 2G040210GS, XikeStor SKS3200M-4GPY2XF)")
    tb.SetComment(1, "Header positions measured on a photo (+/-0.3 mm): check with calipers before ordering")
    tb.SetComment(2, "Generated by modchip/hardware/make_pcb.py + Freerouting; not built or tested yet")


def via_allowed(net, p):
    return True


def preroute(board):
    """The regulator's switching node and its analogue supply, short and locked"""
    r = pcb.Router(board, OFFSET)
    lx = pcb.pad_pos(board, "U1", 48, OFFSET)
    r.route("VREG_LX", [lx, (44.3, lx[1])], width=0.2)       # narrow past the +3V3 trunk at x 43.75
    r.route("VREG_LX", [(44.3, lx[1]), ("L1", 1)], width=0.4)
    av = pcb.pad_pos(board, "U1", 46, OFFSET)
    r.route("VREG_AVDD", [av, (43.75, av[1]), ("C16", 1)], width=0.2)
    # +3V3 pins on U1's right side (VIN 49, USB_OTP_VDD 53, QSPI_IOVDD 54) sit over the +1V1 region:
    # a trunk just outside the pads joins them to CIN (C13), whose own via reaches the +3V3 plane
    ys = [pcb.pad_pos(board, "U1", n, OFFSET) for n in (54, 53, 49)]
    for p in ys:
        r.route("+3V3", [p, (43.75, p[1])], width=0.2)
    r.route("+3V3", [(43.75, ys[0][1]), (43.75, ys[2][1])], width=0.2)
    r.route("+3V3", [(43.75, ys[1][1]), ("C13", 1)], width=0.2)
    # U1 pin 20 (IOVDD, left row): its via up and out of the way, so XIN (pin 21) can leave straight
    r.route("+3V3", [("U1", 20), (34.4, 9.4), (34.0, 8.75)], width=0.2)
    r.via("+3V3", (34.0, 8.75), drill=VIA[1], size=VIA[0])
    # UART: both head right to the tail; RX takes a lane above the pins, TX drops to B.Cu inside the
    # pad row (on F.Cu RX's lane and U5's top pads wall it in)
    r.route("UART_RX", [("U1", 3), (41.0, 6.0), (43.6, 6.0)], width=0.15)
    r.route("UART_TX", [("U1", 2), (41.4, 7.95)], width=0.15)       # TX goes down to B.Cu at once
    r.via("UART_TX", (41.4, 7.95), drill=VIA[1], size=VIA[0])
    # T8: GP7..GP4 (pins 10..7) fan right to R16, R18, R17, R19 under T8; the leftmost pin takes the
    # highest lane, so no two cross, and all run above the UART lanes
    for pin, ref, lane in ((10, "R16", 4.6), (9, "R18", 4.95), (8, "R17", 5.3), (7, "R19", 5.65)):
        x = pcb.pad_pos(board, "U1", pin, OFFSET)[0]
        px, py = pcb.pad_pos(board, ref, 1, OFFSET)
        r.route(board.FindFootprintByReference("U1").FindPadByNumber(str(pin)).GetNetname(),
                [("U1", pin), (x, lane), (px - 0.6, lane), (px, py)], width=0.15)
    # U1 pin 38 (IOVDD, bottom row) straight down past the ring to its plane via
    r.route("+3V3", [("U1", 38), (39.0, 15.0), (38.8, 15.75)], width=0.2)
    r.via("+3V3", (38.8, 15.75), drill=VIA[1], size=VIA[0])
    # +1V1: U1's DVDD pins (6 top, 23 left, 39 bottom) and VREG_FB (50, right) each drop a via just
    # inside the pad ring, joined by B.Cu tracks round the exposed pad. Laid first, because once the
    # signals have escaped, these pins are boxed in.
    vias = {6: (39.8, 8.0), 23: (36.8, 10.6), 39: (39.4, 13.2), 50: (41.6, 11.8)}
    for pin, v in vias.items():
        r.route("+1V1", [("U1", pin), v], width=0.2)
        r.via("+1V1", v, drill=VIA[1], size=VIA[0])
    # left and bottom of the pad only: the upper right under U1 stays free for the QSPI escapes
    r.route("+1V1", [(39.8, 8.0), (36.8, 8.0), (36.8, 13.2), (41.6, 13.2), (41.6, 11.8)], width=0.3, layer="B.Cu")


def planes(board, rings):
    poly = [sw(p) for p in o.OUTLINE]
    board.SetLayerType(board.GetLayerID("In1.Cu"), pcbnew.LT_POWER)
    board.SetLayerType(board.GetLayerID("In2.Cu"), pcbnew.LT_POWER)
    pcb.zone(board, "GND", board.GetLayerID("In1.Cu"), poly)
    pcb.zone(board, "+3V3", board.GetLayerID("In2.Cu"), poly)
    pcb.fill_zones(board)
    u1 = {f"U1.{p.GetNumber()}" for p in board.FindFootprintByReference("U1").Pads()}
    failed = pcb.fanout(board, {"GND", "+3V3"}, via_d=VIA[0], via_drill=VIA[1], clearance=0.206,
                        skip_refs=("J1", "J2", "J3", "TP1"), prefer_inward=u1, via_keepouts=rings,
                        keepouts=edge_keepouts(0.3), allowed=via_allowed, max_reach=3.0,
                        force=("C13.1",))          # the +3V3 trunk ends on CIN: it still needs its plane via
    print("fanout failed:", failed)
    pcb.fill_zones(board)


def build():
    board = pcb.build_from_schematic(SCH, PCB, dict(PLACEMENT), copper_layers=4,
                                     origin=OFFSET, outline=draw_outline, silk_refs=False,  # refs on F.Fab: no room
                                     silk_ref_min=3.0)   # and no silk on parts under 3 mm (it would touch the pads)
    ds = board.GetDesignSettings()
    ds.SetBoardThickness(pcb.MM(THICKNESS))
    ds.SetAuxOrigin(pcb.vec(*sw(o.T3[0])))
    ds.SetGridOrigin(pcb.vec(*sw(o.T3[0])))
    title(board)
    for g in list(board.FindFootprintByReference("J5").GraphicalItems()):   # its outline runs off the edge
        if g.GetLayer() == pcbnew.F_SilkS:
            board.FindFootprintByReference("J5").Remove(g)
    for ref, holes in FIXED_EXACT.items():
        place_on_holes(board.FindFootprintByReference(ref), holes, bottom=ref != "TP1")
    rings = pcb.escape_rings(board, ["U1"], width=0.8)
    x0, y0 = sw((min(x for x, _ in o.OUTLINE), min(y for _, y in o.OUTLINE)))
    x1, y1 = sw((max(x for x, _ in o.OUTLINE), max(y for _, y in o.OUTLINE)))
    a, b = sw((42.1, 11.3)), sw((52.3, 16.8))
    v1_box = [(a[0], a[1], b[0], b[1])]          # the regulator corner: hand-placed parts only
    # escape corridors round U1 that auto-placed parts must leave clear (switch coordinates):
    # left (XIN..RUN to Y1 and J5), top (GP11..GP0 up to the resistor row), right (QSPI to U5),
    # bottom (LED, SW_SENSE, RESET_DRV and pin 38's via)
    corridors = [(30.0, 8.1, 34.8, 12.6), (31.0, 4.6, 58.0, 6.4), (43.6, 6.6, 49.0, 10.6),
                 (37.0, 15.2, 41.2, 16.4)]
    v1_box += [sw(c[:2]) + sw(c[2:]) for c in corridors]
    failed = pcb.autoplace(board, list(PLACEMENT) + list(FIXED_EXACT), power_nets=POWER_NETS,
                           area=(x0, y0, x1, y1), gap=0.25,
                           keepouts=outside_boxes() + edge_keepouts() + header_keepouts() + rings + v1_box)
    print("autoplace failed:", failed)
    if check_overlaps(board):
        sys.exit("fix the placement")
    reference_drawings(board)
    return board, rings


def main():
    if "--pour" in sys.argv:
        board = pcbnew.LoadBoard(PCB)
        for z in list(board.Zones()):
            if z.GetLayer() in (pcbnew.F_Cu, pcbnew.B_Cu) and not z.GetIsRuleArea():
                board.Delete(z)        # Delete, not Remove (SWIG ownership)
        poly = [sw(p) for p in o.OUTLINE]
        pcb.zone(board, "GND", pcbnew.F_Cu, poly)
        pcb.zone(board, "GND", pcbnew.B_Cu, poly)
        pcb.fill_zones(board)
        # hand-soldered headers and bare pads: out of the assembly files altogether
        for ref in ("J1", "J2", "J3", "J5", "TP1"):
            board.FindFootprintByReference(ref).SetExcludedFromPosFiles(True)
        # the kit leaves symbol descriptions off the footprints; copy them so parity is clean
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
