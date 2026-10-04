#!/usr/bin/env python3
"""Require the application and MCUboot devicetree partition maps to match."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

PARTITION = re.compile(
    r"(?P<node>[a-zA-Z0-9_]+):(?:\s+[a-zA-Z0-9_]+:)*\s*partition@[0-9a-fA-F]+\s*\{"
    r".*?label\s*=\s*\"(?P<label>[^\"]+)\";.*?"
    r"reg\s*=\s*<(?P<offset>0x[0-9a-fA-F]+)\s+(?P<size>0x[0-9a-fA-F]+)>;",
    re.DOTALL,
)


def partitions(path: pathlib.Path) -> dict[str, tuple[int, int]]:
    found = {
        match.group("label"): (int(match.group("offset"), 16), int(match.group("size"), 16))
        for match in PARTITION.finditer(path.read_text())
    }
    if not found:
        raise ValueError(f"no partitions found in {path}")
    return found


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "app",
        type=pathlib.Path,
        nargs="?",
        default=root / "firmware/thread-ring/boards/promicro_nrf52840_nrf52840_uf2.overlay",
    )
    parser.add_argument(
        "mcuboot",
        type=pathlib.Path,
        nargs="?",
        default=root / "firmware/thread-ring/sysbuild/mcuboot.overlay",
    )
    args = parser.parse_args()
    try:
        app = partitions(args.app)
        boot = partitions(args.mcuboot)
    except (OSError, ValueError) as exc:
        print(f"partition check: {exc}", file=sys.stderr)
        return 2
    if app != boot:
        print(f"partition check: maps differ\napp={app}\nmcuboot={boot}", file=sys.stderr)
        return 1
    ordered = sorted(app.items(), key=lambda item: item[1][0])
    for (label, (offset, size)), (next_label, (next_offset, _)) in zip(ordered, ordered[1:]):
        if offset + size > next_offset:
            print(f"partition check: {label} overlaps {next_label}", file=sys.stderr)
            return 1
    print(
        "partition map consistent: "
        + ", ".join(f"{label}@0x{offset:x}+0x{size:x}" for label, (offset, size) in ordered)
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
