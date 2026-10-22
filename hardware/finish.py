#!/usr/bin/env python3
"""After Freerouting: close the last open nets with kicad-agent-kit's grid A* finisher.

  kak.sh py finish.py [board.kicad_pcb]

Joins only (no rip-up): ripping other nets up on this board tends to leave more open than it closes.

Plane nets (GND and +3V3, on the inner layers) are left
to their zones; the finisher only sees tracks, pads and vias.
"""
import pathlib, sys

from kak import astar
import pcbnew

PCB = sys.argv[1] if len(sys.argv) > 1 else str(pathlib.Path(__file__).resolve().parent / "modchip.kicad_pcb")
PLANES = ("GND", "+3V3")

def tidy(board):
    """Drop vias sitting on a same-net plated hole, and track ends that lead nowhere"""
    holes = [(p.GetPosition(), p.GetNetCode()) for fp in board.GetFootprints() for p in fp.Pads()
             if p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH]
    for v in [t for t in board.GetTracks() if t.GetClass() == "PCB_VIA"]:
        if any((v.GetPosition() - h).EuclideanNorm() < 1000 and n == v.GetNetCode() for h, n in holes):
            board.Delete(v)
    tol = 2000      # 2 um
    while True:
        tracks = [t for t in board.GetTracks() if t.GetClass() == "PCB_TRACK"]
        vias = [t for t in board.GetTracks() if t.GetClass() == "PCB_VIA"]
        pads = [p for fp in board.GetFootprints() for p in fp.Pads()]

        def touched(t, pt):
            net, lay = t.GetNetCode(), t.GetLayer()
            if any(v.GetNetCode() == net and v.HitTest(pt) for v in vias):
                return True
            if any(p.GetNetCode() == net and p.IsOnLayer(lay) and p.HitTest(pt) for p in pads):
                return True
            for u in tracks:
                if u is t or u.GetNetCode() != net or u.GetLayer() != lay:
                    continue
                if (u.GetStart() - pt).EuclideanNorm() < tol or (u.GetEnd() - pt).EuclideanNorm() < tol or \
                        u.HitTest(pt, 0):
                    return True
            return False

        dead = [t for t in tracks if not touched(t, t.GetStart()) or not touched(t, t.GetEnd())]
        if not dead:
            return
        for t in dead:
            board.Delete(t)


board = pcbnew.LoadBoard(PCB)
left = astar.finish(board, protect=PLANES, width=0.15, clearance=0.21, via_d=0.5, via_drill=0.2, rounds=2, rip=0)
tidy(board)
pcbnew.ZONE_FILLER(board).Fill(board.Zones())
pcbnew.SaveBoard(PCB, board)
print("still open:", left)
