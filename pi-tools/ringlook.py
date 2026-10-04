#!/usr/bin/env python3
"""ringlook — apply a curated look-book preset to the fleet.

    sudo ringlook.py <preset>       apply (viral; any live node carries it)
    ringlook.py --list              show presets

Presets are the camera-verified recipes from docs/look-book.md. The mode
command spreads virally with auto-priority, so one reachable node is
enough. Targets are tried in inventory order until one answers.
"""

import json
import pathlib
import subprocess
import sys

PRESETS = {
    "calm": ("normal", "The default: solid, slow contemplative scenes"),
    "deepbreath": (
        "normal blink=2000 duty=30 wave=fade",
        "Slow synchronized breathing — the resting state",
    ),
    "surge": (
        "party blink=1300 duty=20 wave=fade",
        "Dramatic deep-and-fast swell over party motion",
    ),
    "shimmer": ("ultra blink=750 duty=60 wave=fade", "Fast dense scenes with a quick gentle pulse"),
    "torrent": ("ultra", "Bare ultra: maximum motion, steady light"),
    "slowwink": (
        "normal blink=1500 duty=80 wave=hard",
        "Calm scenes that blink off for a beat every 1.5 s",
    ),
    "strobeparty": ("party blink=320 duty=50 wave=hard", "The loud one: full-crowd hard flash"),
    "party": ("party", "Plain party mode, no effect"),
    "ultra": ("ultra", "Alias of torrent"),
}

HERE = pathlib.Path(__file__).resolve().parent
RINGCTL = HERE / "ringctl.py"
INVENTORY = HERE.parent / "config" / "rings.json"


def list_presets() -> None:
    for name, (tail, blurb) in PRESETS.items():
        print(f"  {name:12s} mode {tail:42s} {blurb}")


def main() -> int:
    if len(sys.argv) != 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        list_presets()
        return 0
    if sys.argv[1] == "--list":
        list_presets()
        return 0
    preset = sys.argv[1].lower()
    if preset not in PRESETS:
        print(f"ringlook: unknown preset {preset!r}; try --list", file=sys.stderr)
        return 2
    tail = PRESETS[preset][0]
    devices = json.loads(INVENTORY.read_text())["devices"]
    for device in devices:
        name = device["name"]
        probe = subprocess.run(
            [sys.executable, str(RINGCTL), name, "status"],
            capture_output=True,
            text=True,
            timeout=30,
        )
        if probe.returncode != 0:
            continue
        result = subprocess.run(
            [sys.executable, str(RINGCTL), name, "mode", *tail.split()], text=True, timeout=30
        )
        return result.returncode
    print("ringlook: no live node found", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
