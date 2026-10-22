#!/bin/sh
# Rebuild the mod chip from scratch: schematic -> checks -> placement, planes, fanout -> Freerouting ->
# A* finisher -> GND pours -> DRC -> renders. Needs the KiCad 10 flatpak and kicad-agent-kit ($KAK).
#
#   KAK=~/Desktop/Workspace/KiCad/kicad-agent-kit ./build.sh [passes]
set -eu
cd "$(dirname "$0")"
KAK=${KAK:-$HOME/Desktop/Workspace/KiCad/kicad-agent-kit}
K="$KAK/kak.sh"
FP="flatpak run --command=python3 org.kicad.KiCad"
CLI="flatpak run --command=kicad-cli org.kicad.KiCad"
mkdir -p build

$FP make_sch.py
$CLI sch upgrade "$PWD/modchip.kicad_sch" >/dev/null
python3 check_sch.py

$K py make_pcb.py                                     # exits non-zero on overlapping courtyards
$K flat fill modchip.kicad_pcb >/dev/null
cp modchip.kicad_pcb build/placed.kicad_pcb

# Freerouting with analytics and telemetry off (the user's freerouting.json allows them). ROUTE_RUNS > 1
# keeps the run with the fewest open connections (Freerouting is deterministic for one input, so this only
# helps with a changed pass count).
$K flat dsn modchip.kicad_pcb "$PWD/build/modchip.dsn" >/dev/null
# 10 um of margin for Freerouting's rounding (it lands a few um under the rule); DRC checks the real 0.2
sed -i 's/(clearance 200)/(clearance 210)/g' build/modchip.dsn
best=999
for i in $(seq "${ROUTE_RUNS:-1}"); do
    rm -f build/modchip.ses
    "$KAK/tools/jre/bin/java" -Xmx2g -jar "$KAK/tools/freerouting-2.4.1.jar" \
        -de build/modchip.dsn -do build/modchip.ses -mp "${1:-100}" --gui.enabled=false \
        --usage_and_diagnostic_data.disable_analytics=true --profile.allow_telemetry=false > build/freerouting.log 2>&1
    test -s build/modchip.ses || continue
    cp build/placed.kicad_pcb build/try.kicad_pcb
    $K flat ses build/try.kicad_pcb "$PWD/build/modchip.ses" >/dev/null
    open=$($K flat stats build/try.kicad_pcb | sed -n 's/.*unrouted=\([0-9]*\).*/\1/p')
    echo "routing run $i: $open open"
    if [ "$open" -lt "$best" ]; then best=$open; cp build/try.kicad_pcb modchip.kicad_pcb; fi
done
test "$best" -lt 999
cp modchip.kicad_pcb build/routed.kicad_pcb
$K py finish.py
$K py make_pcb.py --pour
$K drc modchip.kicad_pcb | grep -v field_mismatch
$K pcb-png modchip.kicad_pcb F.Cu,B.Cu,Edge.Cuts,F.Silkscreen 40 >/dev/null
$CLI sch export pdf -o "$PWD/schematic.pdf" "$PWD/modchip.kicad_sch" >/dev/null
# JLCPCB: Gerbers + drill zip, BOM and placement (bennymeg's Fabrication Toolkit, via kicad-agent-kit)
rm -rf production
$K jlc modchip.kicad_pcb >/dev/null
rm -rf production/backups
# Mechanical: STL (to 3D-print for a fit check, bottom down) and STEP, assembled parts only, no headers.
# KiCad's library has no QFN-60 model; the QFN-56 7x7 mm one has the same body, so a scratch copy uses it.
mkdir -p mechanical build/mech
cp modchip.kicad_pcb modchip.kicad_pro build/mech/
sed -i 's#QFN-60-1EP_7x7mm_P0.4mm_EP3.4x3.4mm.step#QFN-56-1EP_7x7mm_P0.4mm_EP3.2x3.2mm.step#' build/mech/modchip.kicad_pcb
for fmt in stl step; do
    $CLI pcb export $fmt --force --drill-origin --subst-models --component-filter 'C*,R*,U*,Q*,Y*,D*,L*' \
        -o "$PWD/mechanical/modchip-no-headers.$fmt" "$PWD/build/mech/modchip.kicad_pcb" >/dev/null
done
