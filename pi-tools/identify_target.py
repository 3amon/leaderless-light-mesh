#!/usr/bin/env python3
"""Identify inventoried thread-led-node hardware attached over local USB."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
import urllib.request
from dataclasses import asdict, dataclass, replace

DEFAULT_INVENTORY = pathlib.Path(__file__).resolve().parents[1] / "config/rings.json"
DEFAULT_SERIAL_DIR = pathlib.Path("/dev/serial/by-id")
DEFAULT_DASHBOARD = "http://localhost:8080/api/state"
SERIAL_PATTERNS = (
    ("app", re.compile(r"thread-ring_([A-Za-z0-9]+)-if\d+(?:-port\d+)?$")),
    ("dfu", re.compile(r"nice_nano_([A-Za-z0-9]+)-if\d+(?:-port\d+)?$")),
)


@dataclass(frozen=True)
class AttachedTarget:
    name: str | None
    address: str | None
    firmware_version: str | None
    usb_serial: str
    mode: str
    port: str
    by_id: str


def load_inventory(path: pathlib.Path) -> dict[str, dict[str, object]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    devices = document.get("devices")
    if not isinstance(devices, list):
        raise ValueError("inventory must contain a devices list")

    by_serial: dict[str, dict[str, object]] = {}
    for item in devices:
        if not isinstance(item, dict):
            raise ValueError("inventory devices must be objects")
        serial = item.get("usb_serial")
        if serial is None:
            continue
        if not isinstance(serial, str) or not serial:
            raise ValueError("inventory usb_serial values must be non-empty strings")
        if serial in by_serial:
            raise ValueError(f"duplicate inventory USB serial: {serial}")
        by_serial[serial] = item
    return by_serial


def serial_from_by_id(name: str) -> tuple[str, str] | None:
    for mode, pattern in SERIAL_PATTERNS:
        match = pattern.search(name)
        if match:
            return mode, match.group(1)
    return None


def load_firmware_versions(url: str = DEFAULT_DASHBOARD) -> dict[str, str]:
    """Return the live MCUboot image versions published by ring heartbeats."""
    with urllib.request.urlopen(url, timeout=3) as response:
        document = json.load(response)

    if not isinstance(document, dict):
        raise ValueError("dashboard state must be an object")
    nodes = document.get("nodes")
    if not isinstance(nodes, dict):
        raise ValueError("dashboard state must contain a nodes object")

    versions: dict[str, str] = {}
    for node in nodes.values():
        if not isinstance(node, dict):
            continue
        name = node.get("name")
        version = node.get("img")
        if isinstance(name, str) and name and isinstance(version, str) and version:
            versions[name] = version
    return versions


def identify_targets(
    inventory_path: pathlib.Path = DEFAULT_INVENTORY,
    serial_dir: pathlib.Path = DEFAULT_SERIAL_DIR,
    firmware_versions: dict[str, str] | None = None,
) -> list[AttachedTarget]:
    inventory = load_inventory(inventory_path)
    firmware_versions = firmware_versions or {}
    if not serial_dir.is_dir():
        return []

    attached: dict[tuple[str, str], AttachedTarget] = {}
    for entry in sorted(serial_dir.iterdir()):
        parsed = serial_from_by_id(entry.name)
        if parsed is None:
            continue
        mode, serial = parsed
        item = inventory.get(serial)
        name = str(item["name"]) if item and item.get("name") else None
        target = AttachedTarget(
            name=name,
            address=str(item["address"]) if item and item.get("address") else None,
            firmware_version=(firmware_versions.get(name) if name and mode == "app" else None),
            usb_serial=serial,
            mode=mode,
            port=str(entry.resolve(strict=False)),
            by_id=str(entry),
        )
        attached[(serial, mode)] = target
    return sorted(attached.values(), key=lambda target: (target.name is None, target.name or ""))


def format_target(target: AttachedTarget) -> str:
    identity = target.name or "UNKNOWN TARGET"
    address = f" address={target.address}" if target.address else ""
    firmware = target.firmware_version or "unknown"
    return (
        f"{identity} mode={target.mode} fw={firmware} serial={target.usb_serial}"
        f" port={target.port}{address}"
    )


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Identify thread-led-node hardware plugged into this machine."
    )
    parser.add_argument("--inventory", type=pathlib.Path, default=DEFAULT_INVENTORY)
    parser.add_argument("--serial-dir", type=pathlib.Path, default=DEFAULT_SERIAL_DIR)
    parser.add_argument(
        "--dashboard",
        default=DEFAULT_DASHBOARD,
        help="dashboard state URL used to read live firmware versions",
    )
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        targets = identify_targets(args.inventory, args.serial_dir)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"identify_target: {exc}", file=sys.stderr)
        return 3

    if any(target.name and target.mode == "app" for target in targets):
        try:
            versions = load_firmware_versions(args.dashboard)
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            print(f"identify_target: firmware version unavailable: {exc}", file=sys.stderr)
        else:
            targets = [
                replace(target, firmware_version=versions.get(target.name))
                if target.name and target.mode == "app"
                else target
                for target in targets
            ]

    known = [target for target in targets if target.name is not None]
    unknown = [target for target in targets if target.name is None]
    if args.json:
        print(json.dumps({"targets": [asdict(target) for target in targets]}, indent=2))
    elif not targets:
        print("No thread-led-node USB target found.")
    else:
        for target in targets:
            print(format_target(target))

    if len(known) == 1 and not unknown:
        return 0
    if not targets:
        return 1
    if unknown:
        return 2
    return 2  # Multiple inventoried targets are intentionally ambiguous.


if __name__ == "__main__":
    raise SystemExit(main())
