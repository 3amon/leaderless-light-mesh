#!/usr/bin/env python3
"""Generate the complete JLCPCB fabrication+assembly package for this project.

Fixes two known KiCad -> JLCPCB translation problems at the source:

1. CENTROIDS: `kicad-cli pcb export pos` emits the footprint ORIGIN (pin 1 for
   headers/connectors), but JLCPCB's CPL "Mid X/Y" means the package centroid.
   We compute the bounding-box center of each footprint's pads instead.

2. ROTATIONS: JLCPCB's part models use different zero-rotation conventions
   than KiCad footprints for some packages. ROT_DB maps footprint-name
   substrings to a rotation offset (degrees, added to KiCad's rotation).
   Extend it whenever the JLCPCB placement preview shows a part misoriented.

Usage:  python3 fab_export.py        (from the project directory)
Output: thread-led-node-jlcpcb.zip, assembly/thread-led-node-bom.csv,
        assembly/thread-led-node-cpl.csv
"""

import csv
import os
import subprocess
import sys

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.path.join(HERE, "thread-led-node.kicad_pcb")
NAME = "thread-led-node"

# ref: (LCSC part, human comment, package note)
PARTS = {
    "U2": ("C100024", "SN74LV1T34DBVR", "SOT-23-5"),
    "Q1": ("C15127", "AO3401A", "SOT-23"),
    "Q2": ("C8545", "2N7002", "SOT-23"),
    "F1": ("C126818", "1.1A PTC resettable fuse", "1206"),
    "R1": ("C23138", "330R", "0603"),
    "R2": ("C22935", "1M", "0603"),
    "R3": ("C22935", "1M", "0603"),
    "R4": ("C25803", "100k", "0603"),
    "R5": ("C21190", "1k", "0603"),
    "C1": ("C14663", "100nF", "0603"),
    "D3": ("C2286", "LED red 0603", "0603"),
    "C2": ("C105296", "470uF 16V electrolytic", "CP_Radial_D8.0_P3.5"),
    "J1": ("C144394", "JST B3B-XH-A", "JST_XH_3P_vertical"),
    "J2": ("C126363", "JST PH 2P (W-2001S02P-K000)", "JST_PH_2P_vertical"),
    "SW1": ("C42416242", "Tactile 6x6 THT (SH-6X6X4.3H-CJ)", "SW_PUSH_6mm_THT"),
    "J3": ("C7509522", "Female header 1x12 2.54mm", "PinSocket_1x12_P2.54"),
    "J4": ("C7509522", "Female header 1x12 2.54mm", "PinSocket_1x12_P2.54"),
}

# footprint-name substring -> degrees to ADD to KiCad's rotation for JLCPCB.
# Grow this table from placement-preview evidence, one entry per package type.
ROT_DB = [
    ("PinSocket_1x", 90),
]

GERBER_LAYERS = "F.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts"


def rot_offset(fpid):
    for pat, off in ROT_DB:
        if pat in fpid:
            return off
    return 0


def main():
    board = pcbnew.LoadBoard(BOARD)
    os.makedirs(os.path.join(HERE, "assembly"), exist_ok=True)

    # ---- CPL: pad-bbox centroids + rotation DB ----
    rows = []
    for fp in board.GetFootprints():
        ref = fp.GetReference()
        if ref not in PARTS:
            continue
        xs, ys = [], []
        for pad in fp.Pads():
            p = pad.GetPosition()
            xs.append(p.x / 1e6)
            ys.append(p.y / 1e6)
        cx = (min(xs) + max(xs)) / 2
        cy = (min(ys) + max(ys)) / 2
        rot = (fp.GetOrientationDegrees() + rot_offset(str(fp.GetFPID().GetLibItemName()))) % 360
        layer = "Top" if fp.GetLayer() == pcbnew.F_Cu else "Bottom"
        rows.append((ref, cx, -cy, layer, rot))
    rows.sort()
    cpl = os.path.join(HERE, "assembly", f"{NAME}-cpl.csv")
    with open(cpl, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        for ref, cx, cy, layer, rot in rows:
            w.writerow([ref, f"{cx:.4f}mm", f"{cy:.4f}mm", layer, f"{rot:.1f}"])
    print(f"wrote {cpl} ({len(rows)} parts)")

    # ---- BOM: grouped by LCSC part ----
    groups = {}
    for ref, (lcsc, comment, fpnote) in PARTS.items():
        groups.setdefault((comment, lcsc, fpnote), []).append(ref)
    bom = os.path.join(HERE, "assembly", f"{NAME}-bom.csv")
    with open(bom, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #", "JLCPCB Part #"])
        for (comment, lcsc, fpnote), refs in sorted(groups.items(), key=lambda x: sorted(x[1])[0]):
            w.writerow([comment, ",".join(sorted(refs)), fpnote, lcsc, lcsc])
    print(f"wrote {bom} ({len(groups)} lines)")

    # ---- Gerbers + drill + zip ----
    gdir = os.path.join(HERE, "gerbers")
    subprocess.run(["rm", "-rf", gdir])
    os.makedirs(gdir)
    subprocess.run(
        [
            "kicad-cli",
            "pcb",
            "export",
            "gerbers",
            "--output",
            gdir,
            "--layers",
            GERBER_LAYERS,
            BOARD,
        ],
        check=True,
        capture_output=True,
    )
    subprocess.run(
        [
            "kicad-cli",
            "pcb",
            "export",
            "drill",
            "--output",
            gdir,
            "--format",
            "excellon",
            "--excellon-separate-th",
            "--generate-map",
            "--map-format",
            "gerberx2",
            BOARD,
        ],
        check=True,
        capture_output=True,
    )
    zippath = os.path.join(HERE, f"{NAME}-jlcpcb.zip")
    if os.path.exists(zippath):
        os.remove(zippath)
    files = sorted(os.listdir(gdir))
    subprocess.run(["zip", "-q", zippath] + files, cwd=gdir, check=True)
    print(f"wrote {zippath}")


if __name__ == "__main__":
    sys.exit(main())
