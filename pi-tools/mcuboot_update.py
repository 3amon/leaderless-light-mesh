#!/usr/bin/env python3
"""Install and qualify a reviewed MCUboot update on one inventoried ring.

Run this on the border-router Pi with exactly one inventoried ring attached by
USB. The recovery UF2 updates MCUboot and installs its bound application.
The forward image is then deployed transactionally over Thread. With
``--qualification``, an optional rollback image must become active
unconfirmed and revert to the exact forward image.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import pathlib
import shutil
import struct
import subprocess
import sys

import boot_test as boot
import deploy

ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_INVENTORY = ROOT / "config/rings.json"
RECOVERY_START = 0x26000
RECOVERY_END = 0x90000
SLOT0_START = 0x32000
UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FAMILY_NRF52840 = 0xADA52840


@dataclasses.dataclass(frozen=True)
class RecoveryArtifact:
    path: pathlib.Path
    file_sha256: str
    image: deploy.ImageMetadata
    start: int
    end_exclusive: int


@dataclasses.dataclass(frozen=True)
class UpdateProfile:
    path: pathlib.Path
    recovery: RecoveryArtifact
    forward: deploy.ImageMetadata
    rollback: deploy.ImageMetadata | None


def require_hash(value: object, field: str) -> str:
    text = str(value)
    if len(text) != 64:
        raise ValueError(f"{field} must be 64 hexadecimal characters")
    try:
        bytes.fromhex(text)
    except ValueError as exc:
        raise ValueError(f"{field} must be 64 hexadecimal characters") from exc
    return text.upper()


def integer(value: object, field: str) -> int:
    try:
        return int(value, 0) if isinstance(value, str) else int(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{field} must be an integer or 0x-prefixed integer") from exc


def file_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        raise ValueError(f"cannot read artifact {path}: {exc}") from exc
    return digest.hexdigest()


def signed_image_length(data: bytes) -> int:
    header = struct.Struct("<IIHHIIBBHI4x")
    if len(data) < header.size:
        raise ValueError("embedded recovery image is shorter than its MCUboot header")
    magic, _, header_size, protected_size, payload_size, *_ = header.unpack_from(data)
    if magic != deploy.MCUBOOT_MAGIC:
        raise ValueError("recovery UF2 does not contain a slot-0 MCUboot image")
    protected_end = header_size + payload_size + protected_size
    if protected_end + 4 > len(data):
        raise ValueError("embedded recovery image is truncated before its TLV area")
    _, tlv_total = struct.unpack_from("<HH", data, protected_end)
    length = protected_end + tlv_total
    if tlv_total < 4 or length > len(data):
        raise ValueError("embedded recovery image has an invalid TLV length")
    return length


def inspect_recovery_uf2(path: pathlib.Path) -> tuple[int, int, deploy.ImageMetadata]:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise ValueError(f"cannot read recovery UF2 {path}: {exc}") from exc
    if not raw or len(raw) % 512:
        raise ValueError("recovery UF2 size is not a whole number of blocks")
    count = len(raw) // 512
    payloads: dict[int, bytes] = {}
    for index in range(count):
        block = raw[index * 512 : (index + 1) * 512]
        (
            magic0,
            magic1,
            flags,
            address,
            payload_size,
            block_number,
            declared_count,
            family,
        ) = struct.unpack_from("<IIIIIIII", block)
        end_magic = struct.unpack_from("<I", block, 508)[0]
        if (magic0, magic1, end_magic) != (
            UF2_MAGIC_START0,
            UF2_MAGIC_START1,
            UF2_MAGIC_END,
        ):
            raise ValueError(f"recovery UF2 block {index} has invalid magic")
        if (
            flags != 0x2000
            or payload_size != 256
            or block_number != index
            or declared_count != count
            or family != UF2_FAMILY_NRF52840
        ):
            raise ValueError(f"recovery UF2 block {index} has invalid metadata")
        if address in payloads:
            raise ValueError(f"recovery UF2 contains duplicate address 0x{address:x}")
        payloads[address] = block[32 : 32 + payload_size]

    addresses = sorted(payloads)
    start = addresses[0]
    end = addresses[-1] + 256
    expected_addresses = list(range(start, end, 256))
    if addresses != expected_addresses:
        raise ValueError("recovery UF2 address coverage is not contiguous")
    contents = b"".join(payloads[address] for address in addresses)
    slot_offset = SLOT0_START - start
    if slot_offset < 0 or slot_offset >= len(contents):
        raise ValueError("recovery UF2 does not cover slot 0")
    embedded = contents[slot_offset:]
    embedded = embedded[: signed_image_length(embedded)]
    metadata = deploy.inspect_image_bytes(embedded, path.resolve())
    return start, end, metadata


def resolve_artifact(profile_path: pathlib.Path, value: object, field: str) -> pathlib.Path:
    path = pathlib.Path(str(value))
    resolved = path if path.is_absolute() else profile_path.parent / path
    if not resolved.is_file():
        raise ValueError(f"{field} artifact does not exist: {resolved}")
    return resolved.resolve()


def bound_image(profile_path: pathlib.Path, raw: object, field: str) -> deploy.ImageMetadata:
    if not isinstance(raw, dict):
        raise ValueError(f"{field} must be an object")
    try:
        path = resolve_artifact(profile_path, raw["artifact"], field)
        expected_file = require_hash(raw["file_sha256"], f"{field}.file_sha256")
        expected_version = deploy.Version.parse(str(raw["version"]))
        expected_image = require_hash(raw["image_hash"], f"{field}.image_hash")
    except KeyError as exc:
        raise ValueError(f"{field} is missing {exc.args[0]}") from exc
    metadata = deploy.inspect_image(path)
    if metadata.file_sha256.upper() != expected_file:
        raise ValueError(f"{field} file SHA-256 does not match profile")
    if metadata.version.tuple() != expected_version.tuple():
        raise ValueError(f"{field} MCUboot version does not match profile")
    if metadata.image_hash != expected_image:
        raise ValueError(f"{field} MCUboot image hash does not match profile")
    return metadata


def load_profile(path: pathlib.Path) -> UpdateProfile:
    try:
        raw = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot load update profile {path}: {exc}") from exc
    if raw.get("schema") != 1:
        raise ValueError("MCUboot update profile must have schema=1")
    recovery_raw = raw.get("recovery")
    if not isinstance(recovery_raw, dict):
        raise ValueError("recovery must be an object")
    try:
        recovery_path = resolve_artifact(path, recovery_raw["artifact"], "recovery")
        expected_file = require_hash(recovery_raw["file_sha256"], "recovery.file_sha256")
        expected_signed_file = require_hash(
            recovery_raw["signed_file_sha256"], "recovery.signed_file_sha256"
        )
        expected_version = deploy.Version.parse(str(recovery_raw["version"]))
        expected_image = require_hash(recovery_raw["image_hash"], "recovery.image_hash")
        expected_start = integer(recovery_raw["start"], "recovery.start")
        expected_end = integer(recovery_raw["end_exclusive"], "recovery.end_exclusive")
    except KeyError as exc:
        raise ValueError(f"recovery is missing {exc.args[0]}") from exc
    if file_sha256(recovery_path).upper() != expected_file:
        raise ValueError("recovery file SHA-256 does not match profile")
    start, end, recovery_image = inspect_recovery_uf2(recovery_path)
    if (expected_start, expected_end) != (RECOVERY_START, RECOVERY_END):
        raise ValueError(
            f"profile recovery range must be exactly 0x{RECOVERY_START:x}..0x{RECOVERY_END:x}"
        )
    if (start, end) != (expected_start, expected_end):
        raise ValueError("recovery UF2 address range does not match profile")
    if recovery_image.file_sha256.upper() != expected_signed_file:
        raise ValueError("embedded recovery image file SHA-256 does not match profile")
    if recovery_image.version.tuple() != expected_version.tuple():
        raise ValueError("embedded recovery image version does not match profile")
    if recovery_image.image_hash != expected_image:
        raise ValueError("embedded recovery image hash does not match profile")

    forward = bound_image(path, raw.get("forward"), "forward")
    rollback_raw = raw.get("rollback")
    rollback = bound_image(path, rollback_raw, "rollback") if rollback_raw else None
    if forward.version.tuple() <= recovery_image.version.tuple():
        raise ValueError("forward image version must be newer than recovery image")
    if rollback and rollback.version.tuple() <= forward.version.tuple():
        raise ValueError("rollback image version must be newer than forward image")
    return UpdateProfile(
        path=path.resolve(),
        recovery=RecoveryArtifact(
            recovery_path,
            expected_file.lower(),
            recovery_image,
            start,
            end,
        ),
        forward=forward,
        rollback=rollback,
    )


def exact_active(
    states: list[dict[str, object]], expected: deploy.ImageMetadata
) -> dict[str, object]:
    active = [state for state in states if state.get("active") is True]
    if len(active) != 1 or not deploy.state_matches(active[0], expected):
        raise RuntimeError(f"active image is not exact {expected.version}: {active}")
    if active[0].get("confirmed") is not True or active[0].get("pending") is True:
        raise RuntimeError(f"active image is not confirmed and non-pending: {active[0]}")
    return active[0]


def validate_rollback_result(
    result_path: pathlib.Path,
    events_path: pathlib.Path,
    rollback: deploy.ImageMetadata,
) -> None:
    result = json.loads(result_path.read_text())
    results = result.get("results")
    if (
        result.get("exit_code") != 1
        or not isinstance(results, list)
        or len(results) != 1
        or results[0].get("classification") != "rollback"
        or results[0].get("ambiguous") is not False
    ):
        raise RuntimeError(f"rollback deployment had an unexpected result: {result}")
    events = [json.loads(line) for line in events_path.read_text().splitlines()]
    observations = [
        event
        for event in events
        if event.get("event") == "candidate_active_unconfirmed"
        and str((event.get("active") or {}).get("hash", "")).upper() == rollback.image_hash
    ]
    if not observations:
        raise RuntimeError("rollback candidate was not observed active and unconfirmed")


def run_logged(command: list[str], log_path: pathlib.Path) -> int:
    print("+ " + " ".join(command), flush=True)
    with log_path.open("w", buffering=1) as log:
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        assert process.stdout is not None
        for line in process.stdout:
            print(line, end="", flush=True)
            log.write(line)
        return process.wait()


def add_shared_options(command: list[str], args: argparse.Namespace) -> None:
    command.extend(
        [
            "--inventory",
            str(args.inventory),
            "--smpmgr",
            str(args.smpmgr),
            "--smp-helper",
            str(args.smp_helper),
        ]
    )
    if args.smp_python:
        command.extend(["--smp-python", str(args.smp_python)])
    if args.control_key_dir:
        command.extend(["--control-key-dir", str(args.control_key_dir)])
    if args.allow_legacy_control:
        command.append("--allow-legacy-control")


def snapshot_profile(profile: UpdateProfile, outdir: pathlib.Path) -> UpdateProfile:
    artifacts = outdir / "artifacts"
    artifacts.mkdir()

    def snapshot(source: pathlib.Path, name: str) -> pathlib.Path:
        destination = artifacts / name
        shutil.copyfile(source, destination)
        if file_sha256(destination) != file_sha256(source):
            raise RuntimeError(f"artifact changed while snapshotting {source}")
        return destination

    recovery_path = snapshot(profile.recovery.path, "recovery.combined.uf2")
    forward_path = snapshot(profile.forward.path, "forward.signed.bin")
    rollback_path = (
        snapshot(profile.rollback.path, "rollback.signed.bin") if profile.rollback else None
    )
    return dataclasses.replace(
        profile,
        recovery=dataclasses.replace(profile.recovery, path=recovery_path),
        forward=dataclasses.replace(profile.forward, path=forward_path),
        rollback=(
            dataclasses.replace(profile.rollback, path=rollback_path)
            if profile.rollback and rollback_path
            else None
        ),
    )


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=pathlib.Path, required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--sentinel", required=True)
    parser.add_argument("--inventory", type=pathlib.Path, default=DEFAULT_INVENTORY)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument(
        "--qualification",
        action="store_true",
        help="also require an active-unconfirmed candidate and successful rollback",
    )
    parser.add_argument(
        "--allow-disruptive",
        action="store_true",
        help="required acknowledgement that this command flashes and reboots hardware",
    )
    parser.add_argument(
        "--smpmgr", type=pathlib.Path, default=pathlib.Path.home() / ".local/bin/smpmgr"
    )
    parser.add_argument("--smp-python", type=pathlib.Path)
    parser.add_argument(
        "--smp-helper",
        type=pathlib.Path,
        default=pathlib.Path(__file__).with_name("smp_transport.py"),
    )
    parser.add_argument("--control-key-dir", type=pathlib.Path)
    parser.add_argument("--allow-legacy-control", action="store_true")
    args = parser.parse_args(argv)
    if not args.allow_disruptive:
        parser.error("--allow-disruptive is required")
    try:
        args.update_profile = load_profile(args.profile)
        devices = {device.name: device for device in deploy.load_inventory(args.inventory)}
        args.target_device = devices[args.target]
        args.sentinel_device = devices[args.sentinel]
    except (ValueError, KeyError) as exc:
        parser.error(str(exc))
    if args.target == args.sentinel:
        parser.error("target and sentinel must be different devices")
    if not args.target_device.usb_serial:
        parser.error("target has no inventoried USB serial")
    if args.qualification and args.update_profile.rollback is None:
        parser.error("--qualification requires a rollback artifact in the profile")
    if args.out.exists():
        parser.error(f"evidence directory already exists: {args.out}")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    events = deploy.EventLog(args.out)
    exit_code = 0
    try:
        profile = snapshot_profile(args.update_profile, args.out)
        target = args.target_device
        sentinel = args.sentinel_device
        inventory = deploy.load_inventory(args.inventory)
        attached = [
            device.name
            for device in inventory
            if device.usb_serial and boot.usb_path(device.usb_serial)
        ]
        if attached != [target.name]:
            raise RuntimeError(
                f"exactly the selected target must be USB-attached; observed {attached}"
            )

        smp_python = args.smp_python or deploy.smp_python_from(args.smpmgr)
        backend = deploy.SubprocessBackend(
            smp_python,
            args.smp_helper,
            control_key_dir=args.control_key_dir,
            allow_legacy_control=args.allow_legacy_control,
        )
        status = backend.status(target, 5)
        sentinel_status = backend.status(sentinel, 5)
        states = backend.states(target, 15)
        baseline = deploy.active_state(states)
        if (
            not status
            or status.get("name") != target.name
            or status.get("infra") != 1
            or not sentinel_status
            or sentinel_status.get("name") != sentinel.name
            or sentinel_status.get("infra") != 1
            or baseline.get("confirmed") is not True
            or baseline.get("pending") is True
        ):
            raise RuntimeError("target/sentinel/baseline preflight failed")
        events.emit(
            "preflight_passed",
            target=target.name,
            sentinel=sentinel.name,
            status=status,
            sentinel_status=sentinel_status,
            states=states,
        )

        recovery_out = args.out / "recovery"
        recovery_command = [
            sys.executable,
            str(pathlib.Path(__file__).with_name("uf2_recovery_test.py")),
            "--artifact",
            str(profile.recovery.path),
            "--artifact-sha256",
            profile.recovery.file_sha256,
            "--target",
            target.name,
            "--address",
            target.address,
            "--usb-serial",
            str(target.usb_serial),
            "--sentinel",
            sentinel.name,
            "--sentinel-address",
            sentinel.address,
            "--active-version",
            str(profile.recovery.image.version),
            "--active-hash",
            profile.recovery.image.image_hash,
            "--preflight-version",
            str(baseline["version"]),
            "--preflight-hash",
            str(baseline["hash"]),
            "--out",
            str(recovery_out),
        ]
        add_shared_options(recovery_command, args)
        if run_logged(recovery_command, args.out / "recovery.log") != 0:
            raise RuntimeError("native recovery step failed")
        exact_active(backend.states(target, 15), profile.recovery.image)
        events.emit("recovery_verified", image=profile.recovery.image.as_dict())

        erase_command = [
            str(smp_python),
            str(args.smp_helper),
            "--address",
            target.address,
            "--timeout",
            "30",
            "erase",
            "1",
        ]
        if run_logged(erase_command, args.out / "erase-after-recovery.log") != 0:
            raise RuntimeError("could not erase slot 1 after recovery")

        forward_out = args.out / "forward"
        forward_command = [
            sys.executable,
            str(pathlib.Path(__file__).with_name("deploy.py")),
            "deploy",
            target.name,
            str(profile.forward.path),
            "--out",
            str(forward_out),
        ]
        add_shared_options(forward_command, args)
        if run_logged(forward_command, args.out / "forward.log") != 0:
            raise RuntimeError("forward OTA step failed")
        exact_active(backend.states(target, 15), profile.forward)
        events.emit("forward_verified", image=profile.forward.as_dict())

        expected_reset = "ota"
        if args.qualification:
            assert profile.rollback is not None
            rollback_out = args.out / "rollback"
            rollback_command = [
                sys.executable,
                str(pathlib.Path(__file__).with_name("deploy.py")),
                "deploy",
                target.name,
                str(profile.rollback.path),
                "--out",
                str(rollback_out),
            ]
            add_shared_options(rollback_command, args)
            rollback_code = run_logged(rollback_command, args.out / "rollback.log")
            if rollback_code != 1:
                raise RuntimeError(f"rollback deployment returned {rollback_code}, expected 1")
            validate_rollback_result(
                rollback_out / "result.json",
                rollback_out / "events.jsonl",
                profile.rollback,
            )
            exact_active(backend.states(target, 15), profile.forward)
            events.emit(
                "rollback_verified",
                candidate=profile.rollback.as_dict(),
                restored=profile.forward.as_dict(),
            )
            expected_reset = "rollback"

        if run_logged(erase_command, args.out / "erase-final.log") != 0:
            raise RuntimeError("could not erase final slot-1 image")
        final_states = backend.states(target, 15)
        final_active = exact_active(final_states, profile.forward)
        if len(final_states) != 1:
            raise RuntimeError(f"slot 1 is not empty after final cleanup: {final_states}")
        final_status = backend.status(target, 5)
        sentinel_status = backend.status(sentinel, 5)
        boot_status = boot.parse_boot_status(boot.udp_command(target.address, "boot", 5))
        if (
            not final_status
            or final_status.get("name") != target.name
            or final_status.get("infra") != 1
            or str(final_status.get("reset", "")).split("/", 1)[-1] != expected_reset
            or not sentinel_status
            or sentinel_status.get("name") != sentinel.name
            or sentinel_status.get("infra") != 1
            or not boot_status
            or boot_status.get("parse_error")
            or boot_status.get("stage") != "confirmed"
            or boot_status.get("mcuboot_phase") != 0
            or boot_status.get("mcuboot_detail") != 0
        ):
            raise RuntimeError("final boot/status/sentinel verification failed")
        summary = {
            "exit_code": 0,
            "target": target.name,
            "qualification": args.qualification,
            "active": final_active,
            "status": final_status,
            "boot": boot_status,
            "sentinel": sentinel_status,
        }
        events.emit("update_verified", **summary)
        events.write_json("result.json", summary)
    except Exception as exc:
        exit_code = 2
        events.emit("fatal", error=str(exc), type=type(exc).__name__)
        events.write_json(
            "result.json",
            {
                "exit_code": exit_code,
                "target": args.target,
                "qualification": args.qualification,
                "error": f"{type(exc).__name__}: {exc}",
            },
        )
    finally:
        events.close()
    print(f"evidence: {args.out}")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
