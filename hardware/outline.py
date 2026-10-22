#!/usr/bin/env python3
"""Mod-chip board outline for the SWTG024AS-V2.0: generate it and check it.

Everything here is in millimetres, in switch-board coordinates seen from the
top with the ports at the front: x from the left edge, y from the rear edge.
The positions were measured on the board photo in
doc/devices/photos/SWTG024AS-v2.0-unmanaged/, calibrated on U8's 1.27 mm pin
pitch (23.5 px/mm). Expect +/-0.5 mm near the centre of the photo and more
towards its edges. Check them against a real board with the 1:1 print
before ordering anything.

Outputs, next to this script:
  modchip-outline.dxf  Edge.Cuts and header holes, origin at T3 pin 1, Y up,
                       for KiCad (File > Import > Graphics, layer Edge.Cuts)
  modchip-outline.svg  1:1 template: print at 100 % and lay it on the switch
  overlay.jpg          the outline drawn on the board photo (with --photo)
  overlay-managed.jpg  the same on a managed board's photo (--calibration managed)

The script fails if the outline comes closer than CLEARANCE to anything it
must avoid, or leaves a header hole uncovered.
"""
import math, pathlib, sys

HERE = pathlib.Path(__file__).resolve().parent
CLEARANCE = 1.2      # outline to obstacles; more than the photo's error in most places
HOLE_MARGIN = 1.5    # header hole centre to the outline

BOARD = (130.7, 95.2)

# Mod-chip outline, clockwise from the rear-left corner
OUTLINE = [
    (22.5, 0.5),     # right of the DC jack J1
    (61.6, 0.5),     # left of C124
    (61.6, 10.8),    # under C124 and C123
    (80.1, 10.8),    # left of the 1R5 inductor
    (80.1, 18.2),    # under the inductor
    (112.5, 18.2),   # left of EC1
    (112.5, 29.0),   # under EC1
    (124.1, 29.0),
    (124.1, 25.4),   # up to T5, right of EC1
    (129.2, 25.4),
    (129.2, 37.5),   # T5 tail, clear of the J2 SFP cage
    (101.0, 37.5),
    (101.0, 24.5),
    (74.0, 24.5),    # right of the heatsink
    (74.0, 16.2),
    (22.5, 16.2),    # over the heatsink's rear edge, clear of U8's clip
]

# Header holes the mod chip mates with
PITCH_T3 = 2.0
T3 = [(31.69 + PITCH_T3 * i, 2.75) for i in range(6)]
T8 = [(31.69 + PITCH_T3 * (7 + i), 2.75) for i in range(6)]
T5 = [(126.2, 27.14 + 2.54 * i) for i in range(4)]
RESET_PAD = (59.8, 14.0)        # above D3, where the reset line is reached

# Things to stay clear of: through-hole parts, and SMD parts too tall to sit
# under the board (taller than the 4.5 mm stack). ("rect", x0, y0, x1, y1) or
# ("circle", cx, cy, r): the footprint on the PCB, not the photo's view of
# its top, which perspective shifts away from the photo's centre.
OBSTACLES = {
    "J1 DC jack (THT)": ("rect", 7.9, -2.4, 21.15, 13.8),
    "C101 electrolytic (THT)": ("circle", 4.0, 21.5, 5.0),
    "mounting standoff": ("circle", 4.2, 12.7, 3.0),
    "EC2 electrolytic (THT)": ("rect", 12.3, 26.7, 24.8, 35.9),
    "L13 power inductor": ("rect", 10.2, 20.4, 17.7, 26.3),
    "C124 electrolytic": ("rect", 62.9, -0.4, 71.7, 9.44),
    "C123 electrolytic": ("rect", 72.3, -0.4, 81.0, 9.44),
    "1R5 power inductor": ("rect", 81.4, 1.2, 95.2, 16.9),
    "heatsink": ("rect", 49.5, 17.5, 72.7, 40.5),
    "C102 electrolytic (THT)": ("circle", 118.3, 13.0, 4.5),
    "EC1 electrolytic (THT)": ("circle", 118.3, 23.2, 4.5),
    "J4 SFP cage (THT)": ("rect", 81.8, 43.6, 99.5, 95.2),
    "J2 SFP cage (THT)": ("rect", 110.1, 43.6, 128.6, 95.2),
}

# Room for a 208-mil SOIC-8 test clip on the SPI flash U8 (body 28.5-33.6 x
# 26.0-31.1, leads 26.6-35.5): about 4 mm beyond the leads and 4 mm beyond the
# body, and nothing above it
KEEPOUTS = {
    "U8 flash clip access": ("rect", 23.0, 22.0, 39.0, 35.0),
}

# Parts under the board, for the record: all SMD and under 3 mm
UNDER = "L2, L3, C38, C41, U4 and U10 (empty), R53, C103, D3, R78, C121, C129, C125, D2, Q1, Q2, Y1, R/C arrays"


def seg_dist(p, q, a, b):
    """Shortest distance between segments pq and ab"""
    def pt_seg(c, s0, s1):
        dx, dy = s1[0] - s0[0], s1[1] - s0[1]
        L = dx * dx + dy * dy
        t = 0 if L == 0 else max(0, min(1, ((c[0] - s0[0]) * dx + (c[1] - s0[1]) * dy) / L))
        return math.hypot(c[0] - s0[0] - t * dx, c[1] - s0[1] - t * dy)

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    d1, d2 = cross(a, b, p), cross(a, b, q)
    d3, d4 = cross(p, q, a), cross(p, q, b)
    if d1 * d2 < 0 and d3 * d4 < 0:
        return 0.0
    return min(pt_seg(p, a, b), pt_seg(q, a, b), pt_seg(a, p, q), pt_seg(b, p, q))


def inside(pt, poly):
    x, y = pt
    n, c = len(poly), False
    for i in range(n):
        (x0, y0), (x1, y1) = poly[i], poly[(i + 1) % n]
        if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
            c = not c
    return c


def edges(poly):
    return [(poly[i], poly[(i + 1) % len(poly)]) for i in range(len(poly))]


def rect_poly(x0, y0, x1, y1):
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]


def clearance(shape, poly):
    """Distance from the outline to a shape; 0 if they overlap"""
    if shape[0] == "rect":
        r = rect_poly(*shape[1:])
        if any(inside(v, poly) for v in r) or any(inside(v, r) for v in poly):
            return 0.0
        return min(seg_dist(p, q, a, b) for p, q in edges(poly) for a, b in edges(r))
    _, cx, cy, rad = shape
    if inside((cx, cy), poly):
        return 0.0
    d = min(seg_dist((cx, cy), (cx, cy), a, b) for a, b in edges(poly))
    return max(0.0, d - rad)


def area(poly):
    return abs(sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in edges(poly))) / 2


def check():
    ok = True
    print("Clearances (minimum %.1f mm):" % CLEARANCE)
    for name, shape in list(OBSTACLES.items()) + list(KEEPOUTS.items()):
        c = clearance(shape, OUTLINE)
        flag = "ok" if c >= CLEARANCE else "TOO CLOSE"
        ok &= c >= CLEARANCE
        print("  %-28s %6.2f mm  %s" % (name, c, flag))
    print("Header holes (minimum %.1f mm to the edge):" % HOLE_MARGIN)
    for label, holes in (("T3", T3), ("T8", T8), ("T5", T5), ("reset pad", [RESET_PAD])):
        m = min(min(seg_dist(h, h, a, b) for a, b in edges(OUTLINE)) if inside(h, OUTLINE) else -1
                for h in holes)
        flag = "ok" if m >= HOLE_MARGIN else "UNCOVERED" if m < 0 else "TOO CLOSE"
        ok &= m >= HOLE_MARGIN
        print("  %-10s %6.2f mm  %s" % (label, m, flag))
    xs, ys = [p[0] for p in OUTLINE], [p[1] for p in OUTLINE]
    print("Outline: %.1f x %.1f mm extent, %.0f mm^2" % (max(xs) - min(xs), max(ys) - min(ys), area(OUTLINE)))
    return ok


def to_local(p):
    """Switch coordinates -> mod-chip coordinates: origin T3 pin 1, Y up"""
    return (round(p[0] - T3[0][0], 3), round(T3[0][1] - p[1], 3))


def write_dxf(path):
    out = ["0", "SECTION", "2", "ENTITIES"]

    def line(a, b, layer):
        out.extend(["0", "LINE", "8", layer, "10", str(a[0]), "20", str(a[1]), "30", "0",
                    "11", str(b[0]), "21", str(b[1]), "31", "0"])

    def circle(c, r, layer):
        out.extend(["0", "CIRCLE", "8", layer, "10", str(c[0]), "20", str(c[1]), "30", "0", "40", str(r)])

    for a, b in edges(OUTLINE):
        line(to_local(a), to_local(b), "Edge.Cuts")
    for h in T3 + T8:
        circle(to_local(h), 0.45, "Holes_2.0mm")      # 2.0 mm header pins, 0.9 mm drill
    for h in T5:
        circle(to_local(h), 0.5, "Holes_2.54mm")      # 2.54 mm header pins, 1.0 mm drill
    circle(to_local(RESET_PAD), 0.75, "Reset_pad")
    out.extend(["0", "ENDSEC", "0", "EOF"])
    path.write_text("\n".join(out) + "\n")


def write_svg(path):
    W, H = BOARD
    m = 8
    s = []
    s.append('<svg xmlns="http://www.w3.org/2000/svg" width="%gmm" height="%gmm" viewBox="%g %g %g %g">'
             % (W + 2 * m, H + 2 * m + 14, -m, -m, W + 2 * m, H + 2 * m + 14))
    s.append('<defs><pattern id="h" width="2" height="2" patternUnits="userSpaceOnUse" '
             'patternTransform="rotate(45)"><line x1="0" y1="0" x2="0" y2="2" stroke="#c00" stroke-width="0.4"/>'
             '</pattern></defs>')
    s.append('<rect x="0" y="0" width="%g" height="%g" fill="none" stroke="#888" stroke-width="0.3"/>' % (W, H))
    s.append('<text x="1" y="%g" font-size="2.2" fill="#888">SWTG024AS-V2.0 PCB edge (photo estimate), '
             'ports at the bottom</text>' % (H - 1))

    def shape(sh, fill, stroke):
        if sh[0] == "rect":
            x0, y0, x1, y1 = sh[1:]
            return '<rect x="%g" y="%g" width="%g" height="%g" fill="%s" stroke="%s" stroke-width="0.25"/>' % (
                x0, y0, x1 - x0, y1 - y0, fill, stroke)
        return '<circle cx="%g" cy="%g" r="%g" fill="%s" stroke="%s" stroke-width="0.25"/>' % (
            sh[1], sh[2], sh[3], fill, stroke)

    def label_pos(sh):
        return (sh[1] + 0.5, sh[2] + 2.5) if sh[0] == "rect" else (sh[1] - sh[3] + 0.5, sh[2])

    for name, sh in OBSTACLES.items():
        s.append(shape(sh, "url(#h)", "#c00"))
        x, y = label_pos(sh)
        s.append('<text x="%g" y="%g" font-size="1.6" fill="#900">%s</text>' % (x, y, name.split(" (")[0]))
    for name, sh in KEEPOUTS.items():
        s.append(shape(sh, "none", "#06c").replace('stroke-width="0.25"', 'stroke-width="0.4" stroke-dasharray="1.5,1"'))
        x, y = label_pos(sh)
        s.append('<text x="%g" y="%g" font-size="1.6" fill="#06c">%s</text>' % (x, y + 10, name))
    pts = " ".join("%g,%g" % p for p in OUTLINE)
    s.append('<polygon points="%s" fill="rgba(0,160,0,0.12)" stroke="#080" stroke-width="0.4"/>' % pts)
    for h in T3 + T8:
        s.append('<circle cx="%g" cy="%g" r="0.45" fill="none" stroke="#000" stroke-width="0.2"/>' % h)
    for h in T5:
        s.append('<circle cx="%g" cy="%g" r="0.5" fill="none" stroke="#000" stroke-width="0.2"/>' % h)
    s.append('<circle cx="%g" cy="%g" r="0.75" fill="none" stroke="#a0a" stroke-width="0.25"/>' % RESET_PAD)
    s.append('<text x="%g" y="%g" font-size="1.8">T3</text>' % (T3[0][0], T3[0][1] + 3.2))
    s.append('<text x="%g" y="%g" font-size="1.8">T8</text>' % (T8[0][0], T8[0][1] + 3.2))
    s.append('<text x="%g" y="%g" font-size="1.8">T5</text>' % (T5[0][0] - 3.2, T5[0][1] - 1.2))
    s.append('<text x="%g" y="%g" font-size="1.4" fill="#a0a">reset (D3)</text>' % (RESET_PAD[0] - 3, RESET_PAD[1] - 1.2))
    # Scale bar to confirm the print came out at 100 %
    y = H + 6
    s.append('<line x1="0" y1="%g" x2="50" y2="%g" stroke="#000" stroke-width="0.4"/>' % (y, y))
    for x in range(0, 51, 10):
        s.append('<line x1="%d" y1="%g" x2="%d" y2="%g" stroke="#000" stroke-width="0.3"/>' % (x, y - 1.2, x, y))
    s.append('<text x="52" y="%g" font-size="2.2">50 mm: measure this to check the print scale</text>' % (y + 0.8))
    s.append('<text x="0" y="%g" font-size="2.2">Green: mod chip outline. Red: parts to avoid. '
             'Blue: U8 clip access. Black: header holes.</text>' % (y + 5))
    s.append('</svg>')
    path.write_text("\n".join(s) + "\n")


# Calibrations of the photos the outline has been checked against: pixels per
# mm, the board's rear-left corner in pixels, and the rotation that puts the
# ports at the bottom
PHOTOS = {
    # doc/devices/photos/SWTG024AS-v2.0-unmanaged/SWTG024AS-v2.0-top-uman.png,
    # on U8's pin pitch; the photo the outline is measured on
    "unmanaged": (23.5, 152, 81, 0),
    # up-n-atom/SWTG118AS photos/SWGT024AS-v2.0/SWTG024AS-v2-pcb-top.jpg
    # (managed), on U8's pin pitch (38.5) and T3's (38.1)
    "managed": (38.3, 82.5, 113, 180),
}


def write_overlay(photo, path, calibration):
    from PIL import Image, ImageDraw
    ppm, ox, oy, rotation = PHOTOS[calibration]

    def px(p):
        return (ox + p[0] * ppm, oy + p[1] * ppm)

    im = Image.open(photo).convert("RGB").rotate(rotation)
    d = ImageDraw.Draw(im, "RGBA")
    w = max(3, round(ppm * 0.18))      # line widths scale with the photo, about 0.2 mm
    for sh in OBSTACLES.values():
        if sh[0] == "rect":
            d.rectangle([px(sh[1:3]), px(sh[3:5])], outline=(255, 40, 40), width=w)
        else:
            c, r = px(sh[1:3]), sh[3] * ppm
            d.ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r], outline=(255, 40, 40), width=w)
    for sh in KEEPOUTS.values():
        d.rectangle([px(sh[1:3]), px(sh[3:5])], outline=(60, 140, 255), width=w)
    d.polygon([px(p) for p in OUTLINE], fill=(0, 255, 0, 60), outline=(0, 255, 0))
    d.line([px(p) for p in OUTLINE + OUTLINE[:1]], fill=(0, 255, 0), width=round(w * 1.5))
    r = 0.45 * ppm
    for h in T3 + T8 + T5 + [RESET_PAD]:
        c = px(h)
        d.ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r], outline=(255, 255, 0), width=w)
    im = im.resize((1800, int(im.height * 1800 / im.width)))
    im.save(path, quality=85)


if __name__ == "__main__":
    good = check()
    write_dxf(HERE / "modchip-outline.dxf")
    write_svg(HERE / "modchip-outline.svg")
    # --photo FILE [--calibration NAME]: NAME from PHOTOS, default "unmanaged"
    if "--photo" in sys.argv:
        cal = sys.argv[sys.argv.index("--calibration") + 1] if "--calibration" in sys.argv else "unmanaged"
        name = "overlay.jpg" if cal == "unmanaged" else "overlay-%s.jpg" % cal
        write_overlay(sys.argv[sys.argv.index("--photo") + 1], HERE / name, cal)
    print("Local coordinates (origin T3 pin 1, Y up):")
    for p in OUTLINE:
        print("  (%7.2f, %7.2f)" % to_local(p))
    sys.exit(0 if good else 1)
