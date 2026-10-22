# Mod chip board outline

A single rigid board that mates with T3, T8 and T5 on an SWTG024AS-V2.0 and reaches the reset line at
D3. It avoids every through-hole or tall part and leaves the SPI flash U8 free for a test clip.

![outline on the board photo](overlay-managed.jpg)

| File | What |
|---|---|
| `modchip.kicad_pcb`, `modchip.kicad_pro` | KiCad 10 board: the outline on Edge.Cuts, J1-J3 on the bottom side on the T3, T8 and T5 holes, TP1 above D3 for the reset wire, the switch's parts on Dwgs.User, U8's clip zone on Cmts.User. Aux and grid origin at T3 pin 1 |
| `make_pcb.py` | Builds the KiCad board from `outline.py` with KiCad's Python API |
| `outline.py` | The outline, the header holes and the obstacles in one place. Running it checks the clearances and writes the files below |
| `modchip-outline.dxf` | Edge.Cuts, the header holes and the reset pad, in mm, origin at T3 pin 1, Y up |
| `modchip-outline.svg` | 1:1 template with the outline, holes, obstacles and the U8 clip zone |
| `overlay-managed.jpg` | The same drawn on the photo it is measured on: a managed board from [up-n-atom/SWTG118AS](https://github.com/up-n-atom/SWTG118AS/blob/main/photos/SWGT024AS-v2.0/SWTG024AS-v2-pcb-top.jpg) |
| `overlay.jpg` | The same on the unmanaged board's photo in `doc/devices/photos/`, as a cross-check |

Regenerate everything with:

```
python3 outline.py --photo SWTG024AS-v2-pcb-top.jpg --calibration managed   # downloaded from the link above
python3 outline.py --photo ../../doc/devices/photos/SWTG024AS-v2.0-unmanaged/SWTG024AS-v2.0-top-uman.png
```

It exits non-zero if the outline comes closer than 1.2 mm to an obstacle or the U8 clip zone, or if a
header hole sits less than 1.5 mm from the edge.

## KiCad board

`modchip.kicad_pcb` is generated, so change `outline.py` and rebuild it rather than editing the outline
by hand. It needs KiCad 10's Python; with the flatpak:

```
flatpak run --command=python3 org.kicad.KiCad make_pcb.py
```

- **Version.** Saved by KiCad 10.0.5. It opens in any KiCad 10 release, but not in KiCad 9.
- **Footprints.** KiCad's standard 2.0 mm and 2.54 mm pin headers and a 1.5 mm through-hole test point.
- **Pads checked.** After a reload, every pad sits on its measured hole to within 0.001 mm.
- **DRC.** No violations. The only unconnected items are the GND and +3V3_SW pins shared between the
  three headers, which the layout will connect.
- **Nets.** Each pad carries the name of the switch signal it reaches, for example `SW_SCL` and
  `SW_U0TXD`, so the schematic can use the same names.

## Where the numbers come from

**Source.** Everything is measured on up-n-atom's photo of a managed SWTG024AS-V2.0. It's taken
almost straight down, so it is close to a flat scan:

- **Scale.** 38.3 px/mm, from U8's 1.27 mm pin pitch (38.5) and T3's 2.0 mm pitch (38.1).
- **Accuracy.** About ±0.3 mm.
- **Cross-checks on the photo itself.** T5 measures 2.55 mm pitch and the board 130.3 × 88.7 mm. Tall
  parts sit squarely on their footprints, and the round capacitors fill their silkscreen circles.

**Earlier estimate.** A first pass used the unmanaged photo in `doc/devices/photos/`. That photo is taken
at an angle:

- tall parts lean outward;
- the board's depth stretches, to about 95 mm;
- the heatsink appears 1.6 mm further back than it is;
- the header holes come out 0.4-0.8 mm off;
- the rear-right standoff is hidden under C102.

`overlay.jpg` draws the current model on that photo: everything lands within that error.

**Variants.** The managed and unmanaged boards are the same PCB (SWTG024AS-V2.0-17452) and differ only
in fitted parts. The managed one has the reset button K1, T5 without pins, and a W25Q16JV (2 MiB) at
U8 in the same 208-mil SOIC-8 footprint.

## Before ordering a board

1. **Check the print scale.** Print `modchip-outline.svg` at 100 % (A4 landscape fits) and measure the
   50 mm scale bar.
2. **Lay the print on the switch.** Use the bare PCB, with the ports at the bottom of the page. Confirm
   that:
   - the holes fall on T3, T8 and T5;
   - the green outline clears J1, C124/C127, the 1R5 inductor, the heatsink, C102/EC1, the rear standoffs and the SFP cages;
   - the blue zone around U8 is free.
3. **Measure with calipers.** Do this on both switches, the MokerLink and the XikeStor:
   - the T3 pin 1 to T8 pin 6 distance (24.0 mm expected);
   - the T3 pin 1 to T5 pin 1 offset (94.2 mm across, 24.9 mm towards the front, expected);
   - the T5 pitch.
4. **Measure heights.** The lid's inside height over T3/T8 and over the tail, which needs at least
   about 9 mm.
5. **Update and rerun.** Put the measured positions into `outline.py`, run it again, then import the
   DXF into KiCad: File → Import → Graphics, layer Edge.Cuts, units mm.

If the two switches differ, or the tail's fit is too tight to rely on, cut the board at x ≈ 62 mm. That
leaves the main section over T3/T8 and the reset pad, about 39 × 17 mm, and T5 goes back to a 3-wire
harness.
