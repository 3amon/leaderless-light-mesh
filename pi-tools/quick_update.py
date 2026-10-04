#!/usr/bin/env python3
"""Quickly flash one explicitly inventoried USB-connected node through UF2.

This deliberately skips Thread, sentinel, and MCUboot post-boot verification.
It retains the non-negotiable local guards: exactly one connected USB serial
must match inventory, and the dedicated fleet UF2 must match its reviewed
SHA-256 and safe flash range. Use the full deployer for release qualification.
"""

from __future__ import annotations

import argparse
import pathlib
import time

import boot_test as boot
import deploy
import mcuboot_update
import uf2_recovery_test as uf2

DEFAULT_INVENTORY = pathlib.Path(__file__).resolve().parents[1] / "config/rings.json"


def connected_device(inventory: pathlib.Path, target: str | None = None) -> deploy.Device:
    devices = deploy.load_inventory(inventory)
    candidates = [device for device in devices if target is None or device.name == target]
    if target is not None and not candidates:
        raise ValueError(f"target {target!r} is not in inventory")

    connected = []
    for device in candidates:
        if not device.usb_serial:
            continue
        mode = uf2.usb_mode(device.usb_serial)
        block = uf2.block_device_for_serial(device.usb_serial)
        if mode in {"app", "bootloader"} or block is not None:
            connected.append(device)

    if not connected:
        suffix = f" for target {target!r}" if target else ""
        raise ValueError(f"no inventoried USB node is connected{suffix}")
    if len(connected) != 1:
        names = ", ".join(device.name for device in connected)
        raise ValueError(f"quick update requires exactly one connected node; found: {names}")
    return connected[0]


def wait_for_application_usb(args, evidence: boot.Evidence) -> float:
    started = time.monotonic()
    deadline = started + args.return_seconds
    last_mode = None
    while time.monotonic() < deadline:
        mode = uf2.usb_mode(args.usb_serial)
        if mode != last_mode:
            evidence.emit(
                "application_wait",
                elapsed=time.monotonic() - started,
                usb_mode=mode,
                usb_serial=args.usb_serial,
            )
            last_mode = mode
        if mode == "app" and boot.usb_path(args.usb_serial):
            return time.monotonic() - started
        time.sleep(args.poll_interval)
    raise RuntimeError("exact application USB serial did not return")


def parse_args(argv: list[str] | None = None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact", type=pathlib.Path, required=True)
    parser.add_argument("--artifact-sha256", required=True)
    parser.add_argument("--inventory", type=pathlib.Path, default=DEFAULT_INVENTORY)
    parser.add_argument("--target", help="optional inventory name; auto-detected when omitted")
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--bootloader-seconds", type=float, default=15)
    parser.add_argument("--copy-seconds", type=float, default=60)
    parser.add_argument("--return-seconds", type=float, default=60)
    parser.add_argument("--poll-interval", type=float, default=0.25)
    args = parser.parse_args(argv)

    if not args.artifact.is_file():
        parser.error(f"artifact does not exist: {args.artifact}")
    actual_sha = uf2.file_sha256(args.artifact)
    if actual_sha != args.artifact_sha256.lower():
        parser.error("artifact SHA-256 does not match")
    try:
        start, end, image = mcuboot_update.inspect_recovery_uf2(args.artifact)
    except ValueError as exc:
        parser.error(str(exc))
    expected_range = (mcuboot_update.RECOVERY_START, mcuboot_update.RECOVERY_END)
    if (start, end) != expected_range:
        parser.error(
            "fleet quick-update UF2 range must be exactly "
            f"0x{expected_range[0]:x}..0x{expected_range[1]:x}"
        )
    try:
        device = connected_device(args.inventory, args.target)
    except ValueError as exc:
        parser.error(str(exc))
    for field in ("bootloader_seconds", "copy_seconds", "return_seconds", "poll_interval"):
        if getattr(args, field) <= 0:
            parser.error(f"--{field.replace('_', '-')} must be positive")

    args.target = device.name
    args.address = device.address
    args.usb_serial = device.usb_serial
    args.artifact_sha256 = actual_sha
    args.image_metadata = image.as_dict()
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    evidence = boot.Evidence(args.out)
    result = None
    exit_code = 0
    try:
        evidence.write_json(
            "invocation.json",
            {
                "artifact": str(args.artifact.resolve()),
                "artifact_sha256": args.artifact_sha256,
                "inventory": str(args.inventory.resolve()),
                "target": args.target,
                "address": args.address,
                "usb_serial": args.usb_serial,
                "image_metadata": args.image_metadata,
            },
        )
        uf2.cleanup_mount(evidence)
        mode = uf2.usb_mode(args.usb_serial)
        evidence.emit(
            "preflight",
            target=args.target,
            address=args.address,
            usb_serial=args.usb_serial,
            usb_mode=mode,
            artifact_sha256=args.artifact_sha256,
            image=args.image_metadata,
        )
        if mode == "app":
            path = boot.usb_path(args.usb_serial)
            if path is None:
                raise RuntimeError("exact application USB serial disappeared")
            touch = uf2.run(["stty", "-F", path, "1200"])
            evidence.emit(
                "uf2_touch",
                path=path,
                returncode=touch.returncode,
                stdout=touch.stdout,
                stderr=touch.stderr,
            )
        elif mode != "bootloader" and uf2.block_device_for_serial(args.usb_serial) is None:
            raise RuntimeError(f"unexpected USB mode for exact target: {mode}")

        bootloader_at = uf2.wait_for_bootloader(args, evidence, 1)
        copy_duration = uf2.flash_uf2(args, evidence, 1)
        returned_at = wait_for_application_usb(args, evidence)
        result = {
            "classification": "success",
            "target": args.target,
            "address": args.address,
            "usb_serial": args.usb_serial,
            "artifact_sha256": args.artifact_sha256,
            "image": args.image_metadata,
            "bootloader_at": bootloader_at,
            "copy_duration": copy_duration,
            "usb_returned_at": returned_at,
        }
        evidence.emit("quick_update_succeeded", **result)
    except Exception as exc:
        result = {
            "classification": "failed",
            "target": args.target,
            "usb_serial": args.usb_serial,
            "error": str(exc),
            "type": type(exc).__name__,
        }
        evidence.emit("quick_update_failed", **result)
        exit_code = 1
    finally:
        try:
            uf2.cleanup_mount(evidence)
        except Exception as exc:
            evidence.emit("cleanup_error", error=str(exc))
            exit_code = exit_code or 1
        evidence.write_json("result.json", {"exit_code": exit_code, "result": result})
        evidence.close()
    print(f"evidence: {args.out}")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
