#!/usr/bin/env python3
"""Stage and exercise an artifact-bound MCUboot rollback test.

A reviewed profile binds the inventory canary, sentinel, exact confirmed
baseline, signed candidate artifact, file SHA-256, MCUboot version/hash, and
trial count. The harness requires local canary USB, stages without arming,
re-verifies both slots, and only then invokes the update command.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import pathlib
import re
import shutil
import time

import control_client

import boot_test as boot
import deploy

DEFAULT_TARGET = "opal-reef-tide"
DEFAULT_ADDRESS = "fd00:db8:0:1:a022:b022:c022:d022"
DEFAULT_SENTINEL = "feral-tide-light"
DEFAULT_SENTINEL_ADDRESS = "fd00:db8:0:1:a002:b002:c002:d002"
DEFAULT_INVENTORY = pathlib.Path(__file__).resolve().parents[1] / "config/rings.json"


@dataclasses.dataclass(frozen=True)
class RollbackProfile:
    path: pathlib.Path
    target: deploy.Device
    sentinel: deploy.Device
    active_version: str
    active_hash: str
    artifact: deploy.ImageMetadata
    trials: int


def require_hash(value: object, field: str) -> str:
    text = str(value)
    if not re.fullmatch(r"[0-9A-Fa-f]{64}", text):
        raise ValueError(f"{field} must be 64 hexadecimal characters")
    return text.upper()


def load_profile(path: pathlib.Path, inventory_path: pathlib.Path) -> RollbackProfile:
    try:
        raw = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot load rollback profile {path}: {exc}") from exc
    if raw.get("schema") != 1:
        raise ValueError("rollback profile must have schema=1")
    try:
        target_name = str(raw["target"])
        sentinel_name = str(raw["sentinel"])
        baseline = raw["baseline"]
        candidate = raw["candidate"]
        active_version = str(baseline["version"])
        active_hash = require_hash(baseline["image_hash"], "baseline.image_hash")
        expected_file_hash = require_hash(candidate["file_sha256"], "candidate.file_sha256")
        expected_version = deploy.Version.parse(str(candidate["version"]))
        expected_image_hash = require_hash(candidate["image_hash"], "candidate.image_hash")
        trials = int(raw["trials"])
        artifact_value = pathlib.Path(str(candidate["artifact"]))
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError(f"invalid rollback profile: {exc}") from exc
    if not 1 <= trials <= 5:
        raise ValueError("rollback profile trials must be between 1 and 5")
    deploy.Version.parse(active_version)

    devices = {device.name: device for device in deploy.load_inventory(inventory_path)}
    if target_name == sentinel_name:
        raise ValueError("rollback target and sentinel must be different devices")
    try:
        target = devices[target_name]
        sentinel = devices[sentinel_name]
    except KeyError as exc:
        raise ValueError(f"rollback identity is absent from inventory: {exc.args[0]}") from exc
    if not target.canary:
        raise ValueError("rollback target must be the inventoried canary")
    if not target.usb_serial:
        raise ValueError("rollback target has no inventoried USB serial")

    artifact_path = artifact_value if artifact_value.is_absolute() else path.parent / artifact_value
    artifact = deploy.inspect_image(artifact_path)
    if artifact.file_sha256.upper() != expected_file_hash:
        raise ValueError("candidate artifact file SHA-256 does not match profile")
    if artifact.version.tuple() != expected_version.tuple():
        raise ValueError("candidate artifact MCUboot version does not match profile")
    if artifact.image_hash != expected_image_hash:
        raise ValueError("candidate artifact MCUboot hash does not match profile")
    return RollbackProfile(
        path=path.resolve(),
        target=target,
        sentinel=sentinel,
        active_version=active_version,
        active_hash=active_hash,
        artifact=artifact,
        trials=trials,
    )


def snapshot_artifact(args, evidence: boot.Evidence) -> deploy.ImageMetadata:
    destination = evidence.outdir / "candidate.signed.bin"
    shutil.copyfile(args.image_metadata.path, destination)
    snapshot = deploy.inspect_image(destination)
    source = args.image_metadata
    if (
        snapshot.file_sha256 != source.file_sha256
        or snapshot.image_hash != source.image_hash
        or snapshot.version.tuple() != source.version.tuple()
        or snapshot.size != source.size
    ):
        raise RuntimeError("candidate artifact changed while creating the evidence snapshot")
    evidence.emit(
        "artifact_snapshotted",
        source=source.as_dict(),
        snapshot=snapshot.as_dict(),
    )
    return snapshot


def read_image_states(args) -> tuple[list[dict[str, object]], str]:
    smp_python = (
        pathlib.Path(args.smp_python)
        if args.smp_python
        else deploy.smp_python_from(pathlib.Path(args.smpmgr))
    )
    backend = deploy.SubprocessBackend(
        smp_python,
        pathlib.Path(args.smp_helper),
        control_key_dir=args.control_key_dir,
        allow_legacy_control=args.allow_legacy_control,
    )
    device = deploy.Device(args.target, args.address, args.usb_serial, True, 0)
    states = backend.states(device, args.smp_timeout)
    raw = json.dumps({"ok": True, "images": states}, indent=2, sort_keys=True) + "\n"
    return states, raw


def verify_slots(args, states: list[dict[str, object]]) -> None:
    active = [state for state in states if state["active"]]
    if len(active) != 1:
        raise RuntimeError(f"expected one active slot, got {active}")
    if (
        deploy.Version.parse(str(active[0]["version"])).tuple()
        != deploy.Version.parse(args.active_version).tuple()
        or str(active[0]["hash"]).upper() != args.active_hash.upper()
        or not active[0]["confirmed"]
        or active[0]["pending"]
    ):
        raise RuntimeError(
            f"active image is not exact confirmed {args.active_version}/{args.active_hash}: "
            f"{active[0]}"
        )
    candidates = [
        state
        for state in states
        if not state["active"]
        and deploy.Version.parse(str(state["version"])).tuple()
        == deploy.Version.parse(args.candidate_version).tuple()
    ]
    if len(candidates) != 1 or str(candidates[0]["hash"]).upper() != args.candidate_hash.upper():
        raise RuntimeError(
            f"inactive candidate does not match {args.candidate_version}/"
            f"{args.candidate_hash}: {candidates}"
        )
    if candidates[0]["confirmed"] or candidates[0]["pending"]:
        raise RuntimeError(f"candidate is not idle and unconfirmed: {candidates[0]}")


def preflight(args, evidence: boot.Evidence, trial: int) -> None:
    usb_path = boot.usb_path(args.usb_serial)
    status = boot.parse_status(boot.udp_command(args.address, "status", args.command_timeout))
    sentinel = boot.parse_status(
        boot.udp_command(args.sentinel_address, "status", args.command_timeout)
    )
    states, raw = read_image_states(args)
    evidence.emit(
        "preflight",
        trial=trial,
        usb_path=usb_path,
        status=status,
        sentinel=sentinel,
        states=states,
    )
    (evidence.outdir / f"state-before-{trial:03d}.txt").write_text(raw)
    if usb_path is None:
        raise RuntimeError("target canary USB identity is absent")
    if not status or status.get("name") != args.target or status.get("infra") != 1:
        raise RuntimeError("target identity/infrastructure preflight failed")
    if not sentinel or sentinel.get("name") != args.sentinel or sentinel.get("infra") != 1:
        raise RuntimeError("sentinel identity/infrastructure preflight failed")
    verify_slots(args, states)


def stage_candidate(args, evidence: boot.Evidence) -> dict[str, object]:
    usb_path = boot.usb_path(args.usb_serial)
    status = boot.parse_status(boot.udp_command(args.address, "status", args.command_timeout))
    sentinel = boot.parse_status(
        boot.udp_command(args.sentinel_address, "status", args.command_timeout)
    )
    states, raw = read_image_states(args)
    (evidence.outdir / "state-before-stage.txt").write_text(raw)
    evidence.emit(
        "stage_preflight",
        usb_path=usb_path,
        status=status,
        sentinel=sentinel,
        states=states,
        artifact=args.image_metadata.as_dict(),
    )
    if usb_path is None:
        raise RuntimeError("target canary USB identity is absent")
    if not status or status.get("name") != args.target or status.get("infra") != 1:
        raise RuntimeError("target identity/infrastructure stage preflight failed")
    if not sentinel or sentinel.get("name") != args.sentinel or sentinel.get("infra") != 1:
        raise RuntimeError("sentinel identity/infrastructure stage preflight failed")
    active = deploy.active_state(states)
    if (
        deploy.Version.parse(str(active.get("version"))).tuple()
        != deploy.Version.parse(args.active_version).tuple()
        or str(active.get("hash", "")).upper() != args.active_hash.upper()
        or active.get("confirmed") is not True
        or active.get("pending") is True
    ):
        raise RuntimeError("active image does not match the rollback profile baseline")
    if any(state.get("pending") is True for state in states):
        raise RuntimeError("an image is already pending before rollback staging")

    smp_python = (
        pathlib.Path(args.smp_python)
        if args.smp_python
        else deploy.smp_python_from(pathlib.Path(args.smpmgr))
    )
    backend = deploy.SubprocessBackend(
        smp_python,
        pathlib.Path(args.smp_helper),
        control_key_dir=args.control_key_dir,
        allow_legacy_control=args.allow_legacy_control,
    )
    device = deploy.Device(args.target, args.address, args.usb_serial, True, 0)
    with deploy.device_lock(device, args.lock_dir):
        result = deploy.stage_transaction(
            device,
            args.image_metadata,
            backend,
            evidence,
            deploy.DeployConfig(
                command_timeout=args.command_timeout,
                state_timeout=args.smp_timeout,
                upload_timeout=args.upload_timeout,
                upload_attempts=args.upload_attempts,
                upload_retry_delay=args.upload_retry_delay,
                upload_verify_timeout=args.upload_verify_timeout,
                poll_interval=args.poll_interval,
            ),
        )
    states, raw = read_image_states(args)
    verify_slots(args, states)
    (evidence.outdir / "state-after-stage.txt").write_text(raw)
    evidence.emit("stage_complete", result=result, states=states)
    return result


def run_trial(args, evidence: boot.Evidence, capture: boot.SerialCapture, trial: int) -> dict:
    wall_start = boot.utc_now()
    if args.allow_legacy_control:
        reply = boot.udp_command(args.address, "update", args.command_timeout)
    else:
        reply = control_client.authenticated_udp_command(
            args.address,
            args.target,
            "update",
            args.command_timeout,
            key_path=control_client.key_path_for(args.target, args.control_key_dir),
        )
    evidence.emit("update_command", trial=trial, reply=reply)
    if not reply or "armed, rebooting" not in reply:
        raise RuntimeError(f"trial {trial}: update was not acknowledged")

    started = time.monotonic()
    deadline = started + args.max_seconds
    candidate_seen = False
    rollback_seen = False
    saw_status_down = False
    sentinel_failures = 0
    saw_sentinel_down = False
    last_status_raw = object()

    while time.monotonic() < deadline:
        capture.poll()
        status = boot.parse_status(boot.udp_command(args.address, "status", args.command_timeout))
        reason = str(status.get("reset", "")).split("/", 1)[-1] if status else None
        comparable = (bool(status), reason, status and status.get("infra"))
        if comparable != last_status_raw:
            evidence.emit(
                "status",
                trial=trial,
                elapsed=time.monotonic() - started,
                available=bool(status),
                status=status,
            )
            last_status_raw = comparable
        if not status:
            saw_status_down = True
        else:
            candidate_seen |= reason == "ota" and saw_status_down
            rollback_seen |= reason == "rollback" and saw_status_down

        sentinel = boot.parse_status(
            boot.udp_command(args.sentinel_address, "status", args.command_timeout)
        )
        sentinel_ok = bool(
            sentinel and sentinel.get("name") == args.sentinel and sentinel.get("infra") == 1
        )
        sentinel_failures = 0 if sentinel_ok else sentinel_failures + 1
        if sentinel_failures == 3:
            saw_sentinel_down = True
            evidence.emit(
                "sentinel_down",
                trial=trial,
                elapsed=time.monotonic() - started,
                status=sentinel,
            )

        healthy_rollback = bool(
            status
            and status.get("name") == args.target
            and status.get("infra") == 1
            and rollback_seen
            and saw_status_down
        )
        if healthy_rollback:
            break
        time.sleep(args.poll_interval)

    states = []
    raw_state = ""
    state_error = None
    try:
        states, raw_state = read_image_states(args)
        verify_slots(args, states)
    except Exception as exc:
        state_error = str(exc)
    (evidence.outdir / f"state-after-{trial:03d}.txt").write_text(raw_state)
    success = rollback_seen and not saw_sentinel_down and state_error is None
    result = {
        "trial": trial,
        "success": success,
        "duration": time.monotonic() - started,
        "candidate_seen": candidate_seen,
        "rollback_seen": rollback_seen,
        "saw_status_down": saw_status_down,
        "saw_sentinel_down": saw_sentinel_down,
        "states": states,
        "state_error": state_error,
    }
    evidence.emit("trial_result", **result)
    (evidence.outdir / f"kernel-trial-{trial:03d}.log").write_text(
        boot.kernel_log_since(wall_start)
    )
    return result


def parse_args(argv: list[str] | None = None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=pathlib.Path, required=True)
    parser.add_argument("--inventory", type=pathlib.Path, default=DEFAULT_INVENTORY)
    parser.add_argument("--max-seconds", type=float, default=180)
    parser.add_argument("--settle-seconds", type=float, default=10)
    parser.add_argument("--poll-interval", type=float, default=0.25)
    parser.add_argument("--command-timeout", type=float, default=0.2)
    parser.add_argument("--smp-timeout", type=float, default=15)
    parser.add_argument("--upload-timeout", type=float, default=360)
    parser.add_argument("--upload-attempts", type=int, default=3)
    parser.add_argument("--upload-retry-delay", type=float, default=5)
    parser.add_argument("--upload-verify-timeout", type=float, default=45)
    parser.add_argument("--smpmgr", default=str(pathlib.Path.home() / ".local/bin/smpmgr"))
    parser.add_argument("--smp-python")
    parser.add_argument(
        "--smp-helper",
        default=str(pathlib.Path(__file__).resolve().with_name("smp_transport.py")),
    )
    parser.add_argument(
        "--lock-dir", type=pathlib.Path, default=pathlib.Path("/tmp/thread-ring-deploy-locks")
    )
    parser.add_argument("--control-key-dir", type=pathlib.Path)
    parser.add_argument("--allow-legacy-control", action="store_true")
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    try:
        profile = load_profile(args.profile, args.inventory)
    except ValueError as exc:
        parser.error(str(exc))
    args.target = profile.target.name
    args.address = profile.target.address
    args.usb_serial = profile.target.usb_serial
    args.sentinel = profile.sentinel.name
    args.sentinel_address = profile.sentinel.address
    args.active_version = profile.active_version
    args.active_hash = profile.active_hash
    args.candidate_version = str(profile.artifact.version)
    args.candidate_hash = profile.artifact.image_hash
    args.trials = profile.trials
    args.image_metadata = profile.artifact
    for field in (
        "max_seconds",
        "settle_seconds",
        "poll_interval",
        "command_timeout",
        "smp_timeout",
        "upload_timeout",
        "upload_retry_delay",
        "upload_verify_timeout",
    ):
        if getattr(args, field) <= 0:
            parser.error(f"--{field.replace('_', '-')} must be positive")
    if not 1 <= args.upload_attempts <= 6:
        parser.error("--upload-attempts must be between 1 and 6")
    return args


def main() -> int:
    args = parse_args()
    evidence = boot.Evidence(args.out)
    capture = boot.SerialCapture(args.usb_serial, evidence)
    results = []
    exit_code = 0
    try:
        source_metadata = args.image_metadata
        args.image_metadata = snapshot_artifact(args, evidence)
        invocation = {
            key: str(value) if isinstance(value, pathlib.Path) else value
            for key, value in vars(args).items()
            if key != "image_metadata"
        }
        invocation["source_image_metadata"] = source_metadata.as_dict()
        invocation["staged_image_metadata"] = args.image_metadata.as_dict()
        evidence.write_json("invocation.json", invocation)
        stage_candidate(args, evidence)
        for trial in range(1, args.trials + 1):
            preflight(args, evidence, trial)
            result = run_trial(args, evidence, capture, trial)
            results.append(result)
            if not result["success"]:
                evidence.emit("safety_stop", trial=trial, reason="rollback validation failed")
                exit_code = 1
                break
            if trial < args.trials:
                time.sleep(args.settle_seconds)
    except Exception as exc:
        evidence.emit("fatal", error=str(exc), type=type(exc).__name__)
        exit_code = 2
    finally:
        capture.close()
        evidence.write_json("result.json", {"exit_code": exit_code, "trials": results})
        evidence.close()
    print(f"evidence: {args.out}")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
