#!/usr/bin/env python3
"""Reflash one explicitly inventoried USB-connected ring through native UF2."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import time

import boot_test as boot
import deploy
import rollback_test as rollback

APP_USB_ID = "2fe3:0100"
BOOTLOADER_USB_ID = "239a:00b3"
BLOCK_LABEL = "NICENANO"
MOUNTPOINT = "/mnt/thread-ring-nicenano"
DEFAULT_INVENTORY = pathlib.Path(__file__).resolve().parents[1] / "config/rings.json"


def run(command: list[str], timeout: float = 30) -> subprocess.CompletedProcess:
    return subprocess.run(command, text=True, capture_output=True, check=False, timeout=timeout)


def usb_mode(serial: str | None = None) -> str:
    if serial:
        path = boot.usb_path(serial)
        if path is None:
            return "absent"
        properties = run(["udevadm", "info", "--query=property", "--name", path])
        values = {}
        for line in properties.stdout.splitlines():
            key, separator, value = line.partition("=")
            if separator:
                values[key] = value.lower()
        identity = f"{values.get('ID_VENDOR_ID', '')}:{values.get('ID_MODEL_ID', '')}"
        if identity == BOOTLOADER_USB_ID:
            return "bootloader"
        if identity == APP_USB_ID:
            return "app"
        return "unknown"
    output = run(["lsusb"]).stdout.lower()
    if BOOTLOADER_USB_ID in output:
        return "bootloader"
    if APP_USB_ID in output:
        return "app"
    return "absent"


def block_device_for_serial(serial: str) -> str | None:
    result = run(["lsblk", "--json", "-o", "PATH,LABEL,SERIAL,TYPE,MOUNTPOINTS"])
    if result.returncode:
        return None
    try:
        devices = json.loads(result.stdout).get("blockdevices", [])
    except json.JSONDecodeError:
        return None
    matches = [
        str(device["path"])
        for device in devices
        if device.get("type") == "disk"
        and device.get("label") == BLOCK_LABEL
        and str(device.get("serial", "")).strip() == serial
    ]
    if len(matches) > 1:
        raise RuntimeError(f"USB serial {serial!r} matched multiple UF2 disks: {matches}")
    return matches[0] if matches else None


def mounted_paths(device: str) -> list[str]:
    result = run(["lsblk", "--json", "-o", "PATH,MOUNTPOINTS", device])
    if result.returncode:
        raise RuntimeError(f"could not inspect UF2 mount state: {result.stderr}")
    try:
        devices = json.loads(result.stdout).get("blockdevices", [])
    except json.JSONDecodeError as exc:
        raise RuntimeError("lsblk returned invalid JSON") from exc
    if len(devices) != 1 or devices[0].get("path") != device:
        raise RuntimeError(f"could not resolve exact UF2 block device {device}")
    return [str(path) for path in devices[0].get("mountpoints") or [] if path]


def file_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def active_image_ok(states: list[dict[str, object]], version: str, image_hash: str) -> bool:
    active = [state for state in states if state["active"]]
    return bool(
        len(active) == 1
        and active[0]["version"] == version
        and active[0]["hash"] == image_hash.upper()
        and active[0]["confirmed"]
        and not active[0]["pending"]
    )


def recovery_class(status: dict[str, object] | None) -> str | None:
    if not status:
        return None
    reset = str(status.get("reset", ""))
    flags_text, _, reason = reset.partition("/")
    if reason == "uf2":
        return "direct"
    try:
        flags = int(flags_text, 16)
    except ValueError:
        return None
    if flags & 0x10:
        return "watchdog-assisted"
    return None


def validate_inventory_identity(
    inventory: pathlib.Path,
    target: str,
    address: str,
    usb_serial: str,
    sentinel: str,
    sentinel_address: str,
) -> None:
    devices = {device.name: device for device in deploy.load_inventory(inventory)}
    if target == sentinel:
        raise ValueError("target and sentinel must be different devices")
    if target not in devices:
        raise ValueError(f"target {target!r} is not in inventory")
    if sentinel not in devices:
        raise ValueError(f"sentinel {sentinel!r} is not in inventory")
    target_device = devices[target]
    sentinel_device = devices[sentinel]
    if target_device.address.lower() != address.lower():
        raise ValueError("target address does not match inventory")
    if not target_device.usb_serial:
        raise ValueError("target has no USB serial recorded in inventory")
    if target_device.usb_serial != usb_serial:
        raise ValueError("target USB serial does not match inventory")
    if sentinel_device.address.lower() != sentinel_address.lower():
        raise ValueError("sentinel address does not match inventory")


def cleanup_mount(evidence: boot.Evidence) -> None:
    mounted = run(["mountpoint", "-q", MOUNTPOINT]).returncode == 0
    if mounted:
        result = run(["sudo", "umount", "-l", MOUNTPOINT])
        evidence.emit(
            "unmount",
            returncode=result.returncode,
            stdout=result.stdout,
            stderr=result.stderr,
        )
        if result.returncode:
            raise RuntimeError(f"could not unmount stale recovery drive: {result.stderr}")


def preflight(args, evidence: boot.Evidence, trial: int) -> None:
    cleanup_mount(evidence)
    path = boot.usb_path(args.usb_serial)
    status = boot.parse_status(boot.udp_command(args.address, "status", args.command_timeout))
    sentinel = boot.parse_status(
        boot.udp_command(args.sentinel_address, "status", args.command_timeout)
    )
    states, raw = rollback.read_image_states(args)
    evidence.emit(
        "preflight",
        trial=trial,
        usb_path=path,
        usb_mode=usb_mode(args.usb_serial),
        status=status,
        sentinel=sentinel,
        states=states,
    )
    (evidence.outdir / f"state-before-{trial:03d}.txt").write_text(raw)
    if path is None or usb_mode(args.usb_serial) != "app":
        raise RuntimeError("target application USB identity is absent")
    if not status or status.get("name") != args.target or status.get("infra") != 1:
        raise RuntimeError("target identity/infrastructure preflight failed")
    if not sentinel or sentinel.get("name") != args.sentinel or sentinel.get("infra") != 1:
        raise RuntimeError("sentinel identity/infrastructure preflight failed")
    preflight_version = args.preflight_version or args.active_version
    preflight_hash = args.preflight_hash or args.active_hash
    if not active_image_ok(states, preflight_version, preflight_hash):
        raise RuntimeError("active MCUboot image does not match the recovery target")


def wait_for_bootloader(args, evidence: boot.Evidence, trial: int) -> float:
    started = time.monotonic()
    deadline = started + args.bootloader_seconds
    last = None
    while time.monotonic() < deadline:
        mode = usb_mode(args.usb_serial)
        block_device = block_device_for_serial(args.usb_serial)
        state = (mode, block_device)
        if state != last:
            evidence.emit(
                "bootloader_wait",
                trial=trial,
                elapsed=time.monotonic() - started,
                usb_mode=mode,
                block_device=block_device,
            )
            last = state
        if mode == "bootloader" and block_device:
            args.block_device = block_device
            return time.monotonic() - started
        time.sleep(args.poll_interval)
    raise RuntimeError("UF2 USB identity/block label did not appear")


def flash_uf2(args, evidence: boot.Evidence, trial: int) -> float:
    device = getattr(args, "block_device", None)
    if not device or device != block_device_for_serial(args.usb_serial):
        raise RuntimeError("exact UF2 block device disappeared before copy")
    mountpoints = mounted_paths(device)
    if len(mountpoints) > 1:
        raise RuntimeError(f"UF2 block device has multiple mountpoints: {mountpoints}")
    owns_mount = not mountpoints
    destination_root = mountpoints[0] if mountpoints else MOUNTPOINT
    if owns_mount:
        install = run(["sudo", "install", "-d", "-m", "0755", MOUNTPOINT])
        if install.returncode:
            raise RuntimeError(f"could not create recovery mountpoint: {install.stderr}")
        mount = run(["sudo", "mount", "-t", "vfat", device, MOUNTPOINT])
        evidence.emit(
            "mount",
            trial=trial,
            device=device,
            returncode=mount.returncode,
            stdout=mount.stdout,
            stderr=mount.stderr,
        )
        if mount.returncode:
            raise RuntimeError(f"could not mount UF2 drive: {mount.stderr}")
    else:
        evidence.emit("mount_reused", trial=trial, device=device, mountpoint=destination_root)

    started = time.monotonic()
    destination = str(pathlib.Path(destination_root) / args.artifact.name)
    copy = run(["sudo", "cp", str(args.artifact), destination], timeout=args.copy_seconds)
    if copy.returncode:
        raise RuntimeError(f"UF2 copy failed: {copy.stderr}")
    sync = run(["sync"], timeout=args.copy_seconds)
    if sync.returncode:
        raise RuntimeError(f"UF2 sync failed: {sync.stderr}")
    duration = time.monotonic() - started
    evidence.emit("uf2_copy", trial=trial, duration=duration, destination=destination)
    if owns_mount:
        cleanup_mount(evidence)
    return duration


def wait_for_application(args, evidence: boot.Evidence, trial: int) -> tuple[float, dict, str]:
    started = time.monotonic()
    deadline = started + args.recovery_seconds
    last = None
    sentinel_failures = 0
    saw_sentinel_down = False
    while time.monotonic() < deadline:
        mode = usb_mode(args.usb_serial)
        status = boot.parse_status(boot.udp_command(args.address, "status", args.command_timeout))
        classification = recovery_class(status)
        state = (mode, bool(status), classification, status and status.get("infra"))
        if state != last:
            evidence.emit(
                "application_wait",
                trial=trial,
                elapsed=time.monotonic() - started,
                usb_mode=mode,
                status=status,
            )
            last = state

        sentinel = boot.parse_status(
            boot.udp_command(args.sentinel_address, "status", args.command_timeout)
        )
        sentinel_ok = bool(
            sentinel and sentinel.get("name") == args.sentinel and sentinel.get("infra") == 1
        )
        sentinel_failures = 0 if sentinel_ok else sentinel_failures + 1
        if sentinel_failures == 3:
            saw_sentinel_down = True
            evidence.emit("sentinel_down", trial=trial, status=sentinel)

        healthy = bool(
            mode == "app"
            and status
            and status.get("name") == args.target
            and status.get("infra") == 1
            and classification is not None
            and not saw_sentinel_down
        )
        if healthy:
            return time.monotonic() - started, status, classification
        time.sleep(args.poll_interval)
    raise RuntimeError("target did not return healthy from UF2 recovery")


def run_trial(args, evidence: boot.Evidence, trial: int) -> dict:
    wall_start = boot.utc_now()
    path = boot.usb_path(args.usb_serial)
    if path is None:
        raise RuntimeError("USB serial disappeared before 1200-baud touch")
    touch = run(["stty", "-F", path, "1200"])
    evidence.emit(
        "uf2_touch",
        trial=trial,
        path=path,
        returncode=touch.returncode,
        stdout=touch.stdout,
        stderr=touch.stderr,
    )
    # Zephyr may detach the CDC device while stty is still returning, which
    # produces EIO even though the handoff succeeded. Only the independently
    # verified bootloader VID/PID plus block label can accept the transition.
    bootloader_at = wait_for_bootloader(args, evidence, trial)
    copy_duration = flash_uf2(args, evidence, trial)
    recovered_at, status, classification = wait_for_application(args, evidence, trial)
    states, raw = rollback.read_image_states(args)
    (evidence.outdir / f"state-after-{trial:03d}.txt").write_text(raw)
    success = active_image_ok(states, args.active_version, args.active_hash)
    result = {
        "trial": trial,
        "success": success,
        "bootloader_at": bootloader_at,
        "copy_duration": copy_duration,
        "recovered_at": recovered_at,
        "recovery_class": classification,
        "status": status,
        "states": states,
    }
    evidence.emit("trial_result", **result)
    (evidence.outdir / f"kernel-trial-{trial:03d}.log").write_text(
        boot.kernel_log_since(wall_start)
    )
    return result


def parse_args(argv: list[str] | None = None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact", type=pathlib.Path, required=True)
    parser.add_argument("--artifact-sha256", required=True)
    parser.add_argument("--inventory", type=pathlib.Path, default=DEFAULT_INVENTORY)
    parser.add_argument("--target", default=rollback.DEFAULT_TARGET)
    parser.add_argument("--address", default=rollback.DEFAULT_ADDRESS)
    parser.add_argument("--sentinel", default=rollback.DEFAULT_SENTINEL)
    parser.add_argument("--sentinel-address", default=rollback.DEFAULT_SENTINEL_ADDRESS)
    parser.add_argument("--usb-serial", default=boot.DEFAULT_USB_SERIAL)
    parser.add_argument("--active-version", default="0.1.10")
    parser.add_argument(
        "--active-hash",
        default="BDC1F2EB6E59F5950A7432A63FBDD6D6E78E8567138740F16F9B020274B88A90",
    )
    parser.add_argument(
        "--preflight-version",
        help="expected version before trial one (defaults to --active-version)",
    )
    parser.add_argument(
        "--preflight-hash",
        help="expected hash before trial one (defaults to --active-hash)",
    )
    parser.add_argument("--smpmgr", default=str(pathlib.Path.home() / ".local/bin/smpmgr"))
    parser.add_argument("--smp-python")
    parser.add_argument(
        "--smp-helper",
        default=str(pathlib.Path(__file__).resolve().with_name("smp_transport.py")),
    )
    parser.add_argument("--smp-timeout", type=float, default=15)
    parser.add_argument("--control-key-dir", type=pathlib.Path)
    parser.add_argument(
        "--allow-legacy-control",
        action="store_true",
        help="explicit migration-only override for pre-v2 firmware",
    )
    parser.add_argument("--trials", type=int, default=1)
    parser.add_argument("--bootloader-seconds", type=float, default=15)
    parser.add_argument("--copy-seconds", type=float, default=60)
    parser.add_argument("--recovery-seconds", type=float, default=120)
    parser.add_argument("--settle-seconds", type=float, default=10)
    parser.add_argument("--poll-interval", type=float, default=0.25)
    parser.add_argument("--command-timeout", type=float, default=0.2)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    try:
        validate_inventory_identity(
            args.inventory,
            args.target,
            args.address,
            args.usb_serial,
            args.sentinel,
            args.sentinel_address,
        )
    except ValueError as exc:
        parser.error(str(exc))
    if args.trials < 1 or args.trials > 5:
        parser.error("--trials must be between 1 and 5")
    if not args.artifact.is_file():
        parser.error(f"artifact does not exist: {args.artifact}")
    if file_sha256(args.artifact) != args.artifact_sha256.lower():
        parser.error("artifact SHA-256 does not match")
    return args


def main() -> int:
    args = parse_args()
    evidence = boot.Evidence(args.out)
    results = []
    exit_code = 0
    try:
        evidence.write_json(
            "invocation.json",
            vars(args)
            | {
                "out": str(args.out),
                "artifact": str(args.artifact),
                "inventory": str(args.inventory),
            },
        )
        for trial in range(1, args.trials + 1):
            preflight(args, evidence, trial)
            result = run_trial(args, evidence, trial)
            results.append(result)
            if not result["success"]:
                evidence.emit("safety_stop", trial=trial, reason="UF2 verification failed")
                exit_code = 1
                break
            if trial < args.trials:
                time.sleep(args.settle_seconds)
    except Exception as exc:
        evidence.emit("fatal", error=str(exc), type=type(exc).__name__)
        exit_code = 2
    finally:
        try:
            cleanup_mount(evidence)
        except Exception as exc:
            evidence.emit("cleanup_error", error=str(exc))
            exit_code = exit_code or 2
        evidence.write_json("result.json", {"exit_code": exit_code, "trials": results})
        evidence.close()
    print(f"evidence: {args.out}")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
