#!/usr/bin/env python3
"""Outline, header holes and obstacles of the mod chip for the MokerLink POE-2G040210GSM
(PCB 2M-PCB49-V1.1), in one place.

Coordinates are millimetres on the switch board with the origin at T7 pin 1 (the square pad,
towards the front panel), x to the right and y towards the front, as KiCad draws them: T7's pins
run from (0, 0) to (0, -12.7).

Measured on RTLPlayground's doc/devices/photos/2M-PCB49-V1.1/headers.jpg at 16.93 px/mm,
calibrated on T7's and T12's 2.54 mm pitch (the photo is close to square on; expect about
+/-0.5 mm away from T7). T7 is the only header the board mates with; T9 is reached by four wires,
so its position only has to be roughly right.

  python3 outline.py [--photo headers.jpg]

Exits non-zero if the outline comes closer than MIN_CLEAR to an obstacle or a header hole sits
less than MIN_EDGE from the edge. With --photo it draws the result on the photo (overlay.jpg).
"""
import math, pathlib, sys

HERE = pathlib.Path(__file__).resolve().parent
PITCH = 2.54
MIN_CLEAR = 1.0                  # to the obstacles: covers the photo's +/-0.5 mm
MIN_EDGE = 1.5                   # header hole centre to the board edge

# Headers (hole centres). T7: the slave interface, 1x6; the board's J1 mates with it.
T7 = [(0.0, -PITCH * i) for i in range(6)]
# T9: the serial console, 1x4, pin 1 (square) at the rear; wired to the board's J3 pads.
T9 = [(-18.4, -30.7 + PITCH * i) for i in range(4)]
# T10, 3 holes beside T7: covered by the board, so it must stay without pins
T10 = [(-2.86, -11.64 + PITCH * i) for i in range(3)]

# Obstacles: ("rect", x0, y0, x1, y1) or ("circle", cx, cy, r)
OBSTACLES = {
    "right SFP cage (JA4/JA1), tall metal": ("rect", -40.0, -20.3, -5.5, 40.0),
    "screw head (front right of T7)": ("circle", 8.21, 4.08, 3.2),
    "main PCB edge (chassis floor beyond it)": ("rect", 15.6, -60.0, 30.0, 30.0),
    "T11 (6 holes)": ("rect", 4.9, -45.8, 6.9, -30.5),
}

# The board: an 18.9 x 27.5 mm rectangle with the front right corner cut away above the screw head.
OUTLINE = [(-4.3, 1.5), (3.6, 1.5), (3.6, -0.6), (14.6, -0.6), (14.6, -26.0), (-4.3, -26.0)]

# Photo calibration: T7 pin 1 is at this pixel; 16.93 px per mm on both axes
PHOTO_ORIGIN = (1111.0, 1223.5)
PX_PER_MM = 16.93


def edges(poly):
    return list(zip(poly, poly[1:] + poly[:1]))


def inside(p, poly):
    x, y = p
    n = False
    for (x0, y0), (x1, y1) in edges(poly):
        if (y0 > y) != (y1 > y) and x < (x1 - x0) * (y - y0) / (y1 - y0) + x0:
            n = not n
    return n


def seg_point_dist(p, a, b):
    ax, ay = a; bx, by = b; px, py = p
    dx, dy = bx - ax, by - ay
    t = 0.0 if dx == dy == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - ax - t * dx, py - ay - t * dy)


def rect_clearance(poly, r):
    """Smallest distance between the outline and a rectangle, 0 if they overlap"""
    x0, y0, x1, y1 = r
    corners = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
    if any(inside(c, poly) for c in corners) or any(x0 <= x <= x1 and y0 <= y <= y1 for x, y in poly):
        return 0.0
    best = min(seg_point_dist(c, a, b) for c in corners for a, b in edges(poly))
    best = min(best, min(seg_point_dist(p, a, b) for p in poly for a, b in edges(corners)))
    return best


def circle_clearance(poly, c):
    cx, cy, r = c
    if inside((cx, cy), poly):
        return 0.0
    return max(0.0, min(seg_point_dist((cx, cy), a, b) for a, b in edges(poly)) - r)


def check():
    ok = True
    for label, sh in OBSTACLES.items():
        d = rect_clearance(OUTLINE, sh[1:]) if sh[0] == "rect" else circle_clearance(OUTLINE, sh[1:])
        flag = "ok" if d >= MIN_CLEAR else "TOO CLOSE"
        ok &= d >= MIN_CLEAR
        print("%-45s %5.2f mm  %s" % (label, d, flag))
    for i, h in enumerate(T7):
        e = min(seg_point_dist(h, a, b) for a, b in edges(OUTLINE))
        ok &= inside(h, OUTLINE) and e >= MIN_EDGE
        print("T7 pin %d to edge %22s %5.2f mm  %s" % (i + 1, "", e, "ok" if e >= MIN_EDGE else "TOO CLOSE"))
    xs = [p[0] for p in OUTLINE]; ys = [p[1] for p in OUTLINE]
    print("board %.1f x %.1f mm" % (max(xs) - min(xs), max(ys) - min(ys)))
    return ok


def overlay(photo, out):
    from PIL import Image, ImageDraw
    im = Image.open(photo).convert("RGB")
    d = ImageDraw.Draw(im)

    def px(p):
        return (PHOTO_ORIGIN[0] + p[0] * PX_PER_MM, PHOTO_ORIGIN[1] + p[1] * PX_PER_MM)
    for label, sh in OBSTACLES.items():
        if sh[0] == "rect":
            a, b = px(sh[1:3]), px(sh[3:5])
            d.rectangle([max(a[0], 0), max(a[1], 0), min(b[0], im.width - 1), min(b[1], im.height - 1)],
                        outline=(255, 160, 0), width=3)
        else:
            c = px(sh[1:3]); r = sh[3] * PX_PER_MM
            d.ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r], outline=(255, 160, 0), width=3)
    d.polygon([px(p) for p in OUTLINE], outline=(0, 255, 255), width=4)
    for h in T7:
        c = px(h); d.ellipse([c[0] - 9, c[1] - 9, c[0] + 9, c[1] + 9], outline=(255, 0, 255), width=3)
    for h in T9:
        c = px(h); d.ellipse([c[0] - 9, c[1] - 9, c[0] + 9, c[1] + 9], outline=(255, 255, 0), width=3)
    im.save(out, quality=88)
    print("wrote", out)


if __name__ == "__main__":
    good = check()
    if "--photo" in sys.argv:
        overlay(sys.argv[sys.argv.index("--photo") + 1], HERE / "overlay.jpg")
    sys.exit(0 if good else 1)
