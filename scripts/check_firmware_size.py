#!/usr/bin/env python3
"""Check Zephyr firmware flash and RAM use against fleet budgets."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

DEFAULT_FLASH_CAPACITY = 0x5B000  # usable slot-0 FLASH from the linked DTS
DEFAULT_RAM_CAPACITY = 256 * 1024


def parse_size_output(output: str) -> tuple[int, int, int]:
    """Return text, data, and bss values from GNU size's default output."""
    lines = [line.split() for line in output.splitlines() if line.strip()]
    for fields in lines:
        if len(fields) >= 3 and all(value.isdigit() for value in fields[:3]):
            return tuple(int(value) for value in fields[:3])
    raise ValueError("could not parse text/data/bss from size output")


def find_size_tool(explicit: str | None) -> str:
    candidates = [
        explicit,
        os.environ.get("SIZE"),
        "arm-zephyr-eabi-size",
        "size",
    ]
    for candidate in candidates:
        if candidate and (shutil.which(candidate) or os.path.isfile(candidate)):
            return candidate
    raise FileNotFoundError("no GNU size tool found; pass --size-tool")


def percent(used: int, capacity: int) -> float:
    if capacity <= 0:
        raise ValueError("capacity must be positive")
    return used * 100.0 / capacity


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", help="Zephyr application ELF")
    parser.add_argument("--size-tool", help="GNU size-compatible executable")
    parser.add_argument(
        "--flash-capacity",
        type=lambda value: int(value, 0),
        default=DEFAULT_FLASH_CAPACITY,
    )
    parser.add_argument(
        "--ram-capacity",
        type=lambda value: int(value, 0),
        default=DEFAULT_RAM_CAPACITY,
    )
    parser.add_argument("--max-flash-percent", type=float, default=92.0)
    parser.add_argument("--max-ram-percent", type=float, default=80.0)
    args = parser.parse_args()

    if not os.path.isfile(args.elf):
        parser.error(f"ELF not found: {args.elf}")

    try:
        tool = find_size_tool(args.size_tool)
        result = subprocess.run([tool, args.elf], check=True, capture_output=True, text=True)
        text, data, bss = parse_size_output(result.stdout)
    except (FileNotFoundError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"size check error: {exc}", file=sys.stderr)
        return 2

    flash_used = text + data
    ram_used = data + bss
    flash_pct = percent(flash_used, args.flash_capacity)
    ram_pct = percent(ram_used, args.ram_capacity)

    print(
        f"FLASH {flash_used}/{args.flash_capacity} bytes ({flash_pct:.2f}%, "
        f"limit {args.max_flash_percent:.2f}%)"
    )
    print(
        f"RAM   {ram_used}/{args.ram_capacity} bytes ({ram_pct:.2f}%, "
        f"limit {args.max_ram_percent:.2f}%)"
    )

    failures = []
    if flash_pct > args.max_flash_percent:
        failures.append("FLASH")
    if ram_pct > args.max_ram_percent:
        failures.append("RAM")
    if failures:
        print(f"size budget exceeded: {', '.join(failures)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
