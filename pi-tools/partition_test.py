#!/usr/bin/env python3
"""Exercise a bounded physical-USB Thread partition and prove clean rejoin."""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import pathlib
import select
import shutil
import subprocess
import time

import boot_test as boot
import deploy
import rollback_test as rollback

DEFAULT_INVENTORY = pathlib.Path(__file__).resolve().parents[1] / "config/rings.json"


@dataclasses.dataclass(frozen=True)
class PartitionProfile:
    target: deploy.Device
    sentinel: deploy.Device
    image: deploy.ImageMetadata
    duration_seconds: int
    rejoin_timeout_seconds: int


def load_profile(path: pathlib.Path, inventory_path: pathlib.Path) -> PartitionProfile:
    try:
        raw = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot load partition profile {path}: {exc}") from exc
    if raw.get("schema") != 1:
        raise ValueError("partition profile must have schema=1")
    try:
        target_name = str(raw["target"])
        sentinel_name = str(raw["sentinel"])
        image_record = raw["image"]
        artifact_value = pathlib.Path(str(image_record["artifact"]))
        expected_file_hash = rollback.require_hash(image_record["file_sha256"], "image.file_sha256")
        expected_version = deploy.Version.parse(str(image_record["version"]))
        expected_image_hash = rollback.require_hash(image_record["image_hash"], "image.image_hash")
        duration = int(raw["duration_seconds"])
        rejoin_timeout = int(raw["rejoin_timeout_seconds"])
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError(f"invalid partition profile: {exc}") from exc
    if not 10 <= duration <= 300:
        raise ValueError("partition duration must be between 10 and 300 seconds")
    if rejoin_timeout < duration + 30 or rejoin_timeout > 600:
        raise ValueError("rejoin timeout must be 30-600 seconds beyond the partition duration")

    devices = {device.name: device for device in deploy.load_inventory(inventory_path)}
    if target_name == sentinel_name:
        raise ValueError("partition target and sentinel must be different devices")
    try:
        target = devices[target_name]
        sentinel = devices[sentinel_name]
    except KeyError as exc:
        raise ValueError(f"partition identity is absent from inventory: {exc.args[0]}") from exc
    if not target.canary or not target.usb_serial:
        raise ValueError("partition target must be the inventoried USB canary")

    artifact_path = artifact_value if artifact_value.is_absolute() else path.parent / artifact_value
    image = deploy.inspect_image(artifact_path)
    if image.file_sha256.upper() != expected_file_hash:
        raise ValueError("partition artifact file SHA-256 does not match profile")
    if image.version.tuple() != expected_version.tuple():
        raise ValueError("partition artifact MCUboot version does not match profile")
    if image.image_hash != expected_image_hash:
        raise ValueError("partition artifact MCUboot hash does not match profile")
    return PartitionProfile(target, sentinel, image, duration, rejoin_timeout)


def active_image_ok(states: list[dict[str, object]], image: deploy.ImageMetadata) -> bool:
    active = [state for state in states if state.get("active") is True]
    return bool(
        len(active) == 1
        and deploy.state_matches(active[0], image)
        and active[0].get("confirmed") is True
        and active[0].get("pending") is not True
        and not any(state.get("pending") is True for state in states)
    )


def boot_sequence(status: dict[str, object] | None) -> int | None:
    if not status:
        return None
    value = str(status.get("boot", ""))
    head, _, _ = value.partition("/")
    return int(head) if head.isdigit() else None


def converged(status: dict[str, object] | None, target: str) -> bool:
    nbr = str(status.get("nbr", "")) if status else ""
    fields = nbr.split("/")
    try:
        fresh_peers = int(fields[1])
    except (IndexError, ValueError):
        fresh_peers = 0
    return bool(
        status and status.get("name") == target and status.get("infra") == 1 and fresh_peers >= 1
    )


def shell_command(usb_serial: str, command: str, timeout: float = 3) -> str:
    path = boot.usb_path(usb_serial)
    if path is None:
        raise RuntimeError("target canary USB identity is absent")
    configured = subprocess.run(
        ["stty", "-F", path, "115200", "raw", "-echo"],
        text=True,
        capture_output=True,
        check=False,
        timeout=5,
    )
    if configured.returncode:
        raise RuntimeError(f"could not configure USB shell: {configured.stderr.strip()}")
    fd = os.open(path, os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)
    output = bytearray()
    try:
        os.write(fd, b"\r\n" + command.encode("ascii") + b"\r\n")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            readable, _, _ = select.select([fd], [], [], min(0.2, deadline - time.monotonic()))
            if not readable:
                continue
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                continue
            if chunk:
                output.extend(chunk)
                if b"HIL Thread partition active" in output or b"partition cancelled" in output:
                    break
    finally:
        os.close(fd)
    return output.decode(errors="replace")


def read_states(args, profile: PartitionProfile) -> list[dict[str, object]]:
    smp_python = (
        pathlib.Path(args.smp_python)
        if args.smp_python
        else deploy.smp_python_from(pathlib.Path(args.smpmgr))
    )
    backend = deploy.SubprocessBackend(smp_python, pathlib.Path(args.smp_helper))
    return backend.states(profile.target, args.smp_timeout)


def run_partition(args, profile: PartitionProfile, evidence: boot.Evidence) -> dict[str, object]:
    target_status = boot.parse_status(
        boot.udp_command(profile.target.address, "status", args.command_timeout)
    )
    sentinel_status = boot.parse_status(
        boot.udp_command(profile.sentinel.address, "status", args.command_timeout)
    )
    states = read_states(args, profile)
    usb_path = boot.usb_path(profile.target.usb_serial)
    evidence.emit(
        "preflight",
        target=target_status,
        sentinel=sentinel_status,
        states=states,
        usb_path=usb_path,
    )
    if usb_path is None:
        raise RuntimeError("target canary USB identity is absent")
    if not converged(target_status, profile.target.name):
        raise RuntimeError("target identity/infrastructure/group preflight failed")
    if not sentinel_status or sentinel_status.get("name") != profile.sentinel.name:
        raise RuntimeError("sentinel identity preflight failed")
    if sentinel_status.get("infra") != 1:
        raise RuntimeError("sentinel infrastructure preflight failed")
    if not active_image_ok(states, profile.image):
        raise RuntimeError("active image is not the exact confirmed partition-HIL artifact")
    initial_boot = boot_sequence(target_status)
    if initial_boot is None:
        raise RuntimeError("target did not report a parseable boot sequence")

    output = shell_command(
        profile.target.usb_serial,
        f"ring hil-partition {profile.duration_seconds}",
        args.shell_timeout,
    )
    evidence.emit("partition_command", output=output)
    if "HIL Thread partition active" not in output:
        raise RuntimeError("USB shell did not acknowledge the HIL partition")

    started = time.monotonic()
    deadline = started + profile.rejoin_timeout_seconds
    first_down = None
    last_down = None
    consecutive_down = 0
    maximum_down = 0
    sentinel_failures = 0
    final_status = None
    while time.monotonic() < deadline:
        elapsed = time.monotonic() - started
        target = boot.parse_status(
            boot.udp_command(profile.target.address, "status", args.command_timeout)
        )
        sentinel = boot.parse_status(
            boot.udp_command(profile.sentinel.address, "status", args.command_timeout)
        )
        sentinel_ok = bool(
            sentinel
            and sentinel.get("name") == profile.sentinel.name
            and sentinel.get("infra") == 1
        )
        sentinel_failures = 0 if sentinel_ok else sentinel_failures + 1
        if sentinel_failures >= 3:
            raise RuntimeError("sentinel lost infrastructure during target partition")
        if target is None:
            consecutive_down += 1
            maximum_down = max(maximum_down, consecutive_down)
            first_down = elapsed if first_down is None else first_down
            last_down = elapsed
        else:
            consecutive_down = 0
            if (
                maximum_down >= 3
                and last_down is not None
                and last_down - first_down >= profile.duration_seconds - args.outage_slack
                and converged(target, profile.target.name)
            ):
                final_status = target
                break
        time.sleep(args.poll_interval)

    if final_status is None:
        raise RuntimeError("target did not prove a sustained partition and converged rejoin")
    if boot_sequence(final_status) != initial_boot:
        raise RuntimeError("target rebooted during the detach/rejoin test")
    final_states = read_states(args, profile)
    if not active_image_ok(final_states, profile.image):
        raise RuntimeError("partition-HIL image changed or became unconfirmed")
    result = {
        "success": True,
        "duration_seconds": time.monotonic() - started,
        "outage_start_seconds": first_down,
        "outage_end_seconds": last_down,
        "maximum_consecutive_missing": maximum_down,
        "initial_status": target_status,
        "final_status": final_status,
        "initial_boot": initial_boot,
        "final_states": final_states,
    }
    evidence.emit("partition_result", **result)
    return result


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=pathlib.Path, required=True)
    parser.add_argument("--inventory", type=pathlib.Path, default=DEFAULT_INVENTORY)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--poll-interval", type=float, default=0.5)
    parser.add_argument("--command-timeout", type=float, default=0.25)
    parser.add_argument("--shell-timeout", type=float, default=3)
    parser.add_argument("--smp-timeout", type=float, default=15)
    parser.add_argument("--outage-slack", type=float, default=3)
    parser.add_argument("--smpmgr", default=str(pathlib.Path.home() / ".local/bin/smpmgr"))
    parser.add_argument("--smp-python")
    parser.add_argument(
        "--smp-helper",
        default=str(pathlib.Path(__file__).resolve().with_name("smp_transport.py")),
    )
    args = parser.parse_args(argv)
    for field in ("poll_interval", "command_timeout", "shell_timeout", "smp_timeout"):
        if getattr(args, field) <= 0:
            parser.error(f"--{field.replace('_', '-')} must be positive")
    if args.outage_slack < 0 or args.outage_slack > 10:
        parser.error("--outage-slack must be between zero and ten seconds")
    try:
        args.loaded_profile = load_profile(args.profile, args.inventory)
    except ValueError as exc:
        parser.error(str(exc))
    return args


def main() -> int:
    args = parse_args()
    profile = args.loaded_profile
    evidence = boot.Evidence(args.out)
    exit_code = 0
    result = None
    try:
        snapshot_path = evidence.outdir / "partition-hil.signed.bin"
        shutil.copyfile(profile.image.path, snapshot_path)
        snapshot = deploy.inspect_image(snapshot_path)
        if (
            snapshot.file_sha256 != profile.image.file_sha256
            or snapshot.image_hash != profile.image.image_hash
        ):
            raise RuntimeError("partition artifact changed while creating evidence snapshot")
        profile = dataclasses.replace(profile, image=snapshot)
        evidence.write_json(
            "invocation.json",
            {
                "profile": str(args.profile.resolve()),
                "inventory": str(args.inventory.resolve()),
                "target": dataclasses.asdict(profile.target),
                "sentinel": dataclasses.asdict(profile.sentinel),
                "image": profile.image.as_dict(),
                "duration_seconds": profile.duration_seconds,
                "rejoin_timeout_seconds": profile.rejoin_timeout_seconds,
            },
        )
        result = run_partition(args, profile, evidence)
    except Exception as exc:
        evidence.emit("fatal", error=str(exc), type=type(exc).__name__)
        exit_code = 2
    finally:
        if profile.target.usb_serial and boot.usb_path(profile.target.usb_serial):
            try:
                output = shell_command(profile.target.usb_serial, "ring hil-partition cancel")
                evidence.emit("cleanup", output=output)
            except Exception as exc:
                evidence.emit("cleanup_error", error=str(exc))
                exit_code = exit_code or 2
        evidence.write_json("result.json", {"exit_code": exit_code, "result": result})
        evidence.close()
    print(f"evidence: {args.out}")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
