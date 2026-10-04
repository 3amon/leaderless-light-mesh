#!/usr/bin/env python3
"""Transactional firmware deployment for Thread LED rings.

Examples (run on the border-router Pi):

  ./deploy.py inspect-image zephyr.signed.bin
  ./deploy.py stage opal-reef-tide zephyr.signed.bin
  ./deploy.py deploy opal-reef-tide zephyr.signed.bin
  ./deploy.py fleet zephyr.signed.bin

Every mutating operation is identity gated, per-device locked, and recorded as
JSONL.  Success means the exact MCUboot hash is active and confirmed after an
observed reboot; an upload completing is never considered deployment success.
"""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import datetime as dt
import fcntl
import hashlib
import json
import os
import pathlib
import re
import socket
import struct
import subprocess
import sys
import time
from collections.abc import Callable, Iterator
from typing import Protocol

import control_client
import thread_socket

from safe_diagnostics import redact_process_output

MCUBOOT_MAGIC = 0x96F3B83D
TLV_INFO_MAGIC = 0x6907
TLV_PROTECTED_MAGIC = 0x6908
TLV_SHA256 = 0x10
COMMAND_PORT = 4242
DEFAULT_SMP_MTU = 192
MIN_SMP_MTU = 192
MAX_SMP_MTU = 512
DEFAULT_SMP_CHUNK_ATTEMPTS = 3
MAX_SMP_CHUNK_ATTEMPTS = 8
DEFAULT_SMP_UPLOAD_REQUEST_TIMEOUT = 3.0
ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_INVENTORY = ROOT / "config" / "rings.json"


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


@dataclasses.dataclass(frozen=True)
class Version:
    major: int
    minor: int
    revision: int
    build: int = 0

    @classmethod
    def parse(cls, value: str) -> "Version":
        match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)(?:(?:\+|-build|\.)(\d+))?", value)
        if not match:
            raise ValueError(f"invalid MCUboot version {value!r}")
        return cls(*(int(part or 0) for part in match.groups()))

    def tuple(self) -> tuple[int, int, int, int]:
        return self.major, self.minor, self.revision, self.build

    def __str__(self) -> str:
        base = f"{self.major}.{self.minor}.{self.revision}"
        return f"{base}+{self.build}" if self.build else base


@dataclasses.dataclass(frozen=True)
class ImageMetadata:
    path: pathlib.Path
    version: Version
    image_hash: str
    file_sha256: str
    size: int
    header_size: int
    payload_size: int

    def as_dict(self) -> dict[str, object]:
        return {
            "path": str(self.path),
            "version": str(self.version),
            "image_hash": self.image_hash,
            "file_sha256": self.file_sha256,
            "size": self.size,
            "header_size": self.header_size,
            "payload_size": self.payload_size,
        }


def inspect_image(path: pathlib.Path) -> ImageMetadata:
    """Validate an MCUboot signed binary and return its trusted metadata."""
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise ValueError(f"cannot read image {path}: {exc}") from exc
    return inspect_image_bytes(data, path.resolve())


def inspect_image_bytes(data: bytes, path: pathlib.Path) -> ImageMetadata:
    """Validate MCUboot image bytes and return metadata bound to ``path``."""
    header_struct = struct.Struct("<IIHHIIBBHI4x")
    if len(data) < header_struct.size:
        raise ValueError("image is shorter than the MCUboot header")
    (
        magic,
        _load_address,
        header_size,
        protected_size,
        payload_size,
        _flags,
        major,
        minor,
        revision,
        build,
    ) = header_struct.unpack_from(data)
    if magic != MCUBOOT_MAGIC:
        raise ValueError(f"bad MCUboot magic 0x{magic:08x}")
    if header_size < header_struct.size:
        raise ValueError(f"invalid MCUboot header size {header_size}")

    payload_end = header_size + payload_size
    protected_end = payload_end + protected_size
    if protected_end + 4 > len(data):
        raise ValueError("MCUboot image or protected TLV area is truncated")
    if protected_size:
        protected_magic, protected_total = struct.unpack_from("<HH", data, payload_end)
        if protected_magic != TLV_PROTECTED_MAGIC or protected_total != protected_size:
            raise ValueError("invalid MCUboot protected TLV area")

    tlv_magic, tlv_total = struct.unpack_from("<HH", data, protected_end)
    if tlv_magic != TLV_INFO_MAGIC:
        raise ValueError(f"bad MCUboot TLV magic 0x{tlv_magic:04x}")
    tlv_end = protected_end + tlv_total
    if tlv_total < 4 or tlv_end > len(data):
        raise ValueError("MCUboot TLV area is truncated")

    expected_hash = None
    cursor = protected_end + 4
    while cursor < tlv_end:
        if cursor + 4 > tlv_end:
            raise ValueError("truncated MCUboot TLV header")
        tlv_type, tlv_len = struct.unpack_from("<BxH", data, cursor)
        cursor += 4
        if cursor + tlv_len > tlv_end:
            raise ValueError("truncated MCUboot TLV value")
        value = data[cursor : cursor + tlv_len]
        cursor += tlv_len
        if tlv_type == TLV_SHA256:
            if tlv_len != 32 or expected_hash is not None:
                raise ValueError("invalid or duplicate MCUboot SHA256 TLV")
            expected_hash = value
    if expected_hash is None:
        raise ValueError("signed image has no MCUboot SHA256 TLV")
    actual_hash = hashlib.sha256(data[:protected_end]).digest()
    if actual_hash != expected_hash:
        raise ValueError(
            f"MCUboot content hash mismatch: TLV={expected_hash.hex()} "
            f"calculated={actual_hash.hex()}"
        )
    return ImageMetadata(
        path=path,
        version=Version(major, minor, revision, build),
        image_hash=expected_hash.hex().upper(),
        file_sha256=hashlib.sha256(data).hexdigest(),
        size=len(data),
        header_size=header_size,
        payload_size=payload_size,
    )


@dataclasses.dataclass(frozen=True)
class Device:
    name: str
    address: str
    usb_serial: str | None
    canary: bool
    rollout_order: int


def load_inventory(path: pathlib.Path) -> list[Device]:
    try:
        raw = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot load inventory {path}: {exc}") from exc
    if raw.get("schema") != 1 or not isinstance(raw.get("devices"), list):
        raise ValueError("inventory must have schema=1 and a devices list")
    devices: list[Device] = []
    names: set[str] = set()
    addresses: set[str] = set()
    for item in raw["devices"]:
        try:
            device = Device(
                name=str(item["name"]),
                address=str(item["address"]),
                usb_serial=item.get("usb_serial"),
                canary=bool(item.get("canary", False)),
                rollout_order=int(item["rollout_order"]),
            )
        except (KeyError, TypeError, ValueError) as exc:
            raise ValueError(f"invalid inventory record {item!r}: {exc}") from exc
        if ":" not in device.address:
            raise ValueError(f"device {device.name!r} does not have an IPv6 address")
        if device.name in names or device.address.lower() in addresses:
            raise ValueError(f"duplicate inventory identity for {device.name!r}")
        names.add(device.name)
        addresses.add(device.address.lower())
        devices.append(device)
    if sum(device.canary for device in devices) != 1:
        raise ValueError("inventory must identify exactly one canary")
    return sorted(devices, key=lambda device: (device.rollout_order, device.name))


class EventLog:
    def __init__(self, outdir: pathlib.Path):
        outdir.mkdir(parents=True, exist_ok=False)
        self.outdir = outdir
        self._file = (outdir / "events.jsonl").open("w", buffering=1)
        self._start = time.monotonic()

    def emit(self, event: str, **fields) -> dict[str, object]:
        record = {
            "event": event,
            "t": time.monotonic() - self._start,
            "wall": utc_now(),
            **fields,
        }
        self._file.write(json.dumps(record, sort_keys=True) + "\n")
        print(f"[{record['t']:8.3f}] {event}: {fields}", flush=True)
        return record

    def write_json(self, name: str, value: object) -> None:
        (self.outdir / name).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")

    def close(self) -> None:
        self._file.close()


class Backend(Protocol):
    def status(self, device: Device, timeout: float) -> dict[str, object] | None: ...
    def states(self, device: Device, timeout: float) -> list[dict[str, object]]: ...
    def upload(self, device: Device, image: pathlib.Path, timeout: float) -> None: ...
    def update(self, device: Device, timeout: float) -> str | None: ...


def parse_status(reply: str | None) -> dict[str, object] | None:
    if not reply:
        return None
    fields = reply.strip().split()
    if not fields:
        return None
    parsed: dict[str, object] = {"name": fields[0], "raw": reply.strip()}
    for field in fields[1:]:
        if "=" not in field:
            continue
        key, value = field.split("=", 1)
        numeric = re.fullmatch(r"(-?\d+)(?:ms|ppm|s|%)?", value)
        parsed[key] = int(numeric.group(1)) if numeric else value
    return parsed


class SubprocessBackend:
    def __init__(
        self,
        smp_python: pathlib.Path,
        helper: pathlib.Path,
        mtu: int = DEFAULT_SMP_MTU,
        *,
        chunk_attempts: int = DEFAULT_SMP_CHUNK_ATTEMPTS,
        upload_request_timeout: float = DEFAULT_SMP_UPLOAD_REQUEST_TIMEOUT,
        control_key_dir: pathlib.Path | None = None,
        allow_legacy_control: bool = False,
    ):
        self.smp_python = smp_python
        self.helper = helper
        self.mtu = mtu
        self.chunk_attempts = chunk_attempts
        self.upload_request_timeout = upload_request_timeout
        self.control_key_dir = control_key_dir
        self.allow_legacy_control = allow_legacy_control

    def _smp(
        self, device: Device, timeout: float, arguments: list[str], process_timeout: float
    ) -> dict[str, object]:
        command = [
            str(self.smp_python),
            str(self.helper),
            "--address",
            device.address,
            "--mtu",
            str(self.mtu),
            "--timeout",
            str(timeout),
            *arguments,
        ]
        try:
            result = subprocess.run(
                command,
                text=True,
                capture_output=True,
                check=False,
                timeout=process_timeout,
            )
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError(f"SMP command timed out after {process_timeout}s") from exc
        if result.returncode:
            detail = redact_process_output((result.stderr or result.stdout).strip())
            raise RuntimeError(f"SMP command failed ({result.returncode}): {detail}")
        try:
            response = json.loads(result.stdout)
        except json.JSONDecodeError as exc:
            raise RuntimeError(
                "SMP helper returned invalid JSON: "
                f"{redact_process_output(result.stdout, limit=500)!r}"
            ) from exc
        if response.get("ok") is not True:
            raise RuntimeError(f"SMP helper did not report success: {response!r}")
        return response

    def status(self, device: Device, timeout: float) -> dict[str, object] | None:
        sock = thread_socket.udp_socket(device.address, timeout)
        try:
            sock.sendto(b"status", (device.address, COMMAND_PORT))
            return parse_status(sock.recv(512).decode(errors="replace"))
        except (OSError, socket.timeout):
            return None
        finally:
            sock.close()

    def states(self, device: Device, timeout: float) -> list[dict[str, object]]:
        response = self._smp(device, timeout, ["state"], timeout + 5)
        images = response.get("images")
        if not isinstance(images, list):
            raise RuntimeError("SMP state response has no images list")
        return images

    def upload(self, device: Device, image: pathlib.Path, timeout: float) -> None:
        self._smp(
            device,
            min(self.upload_request_timeout, timeout),
            [
                "upload",
                str(image),
                "--first-timeout",
                "45",
                "--chunk-attempts",
                str(self.chunk_attempts),
                "--progress",
            ],
            timeout,
        )

    def update(self, device: Device, timeout: float) -> str | None:
        if not self.allow_legacy_control:
            return control_client.authenticated_udp_command(
                device.address,
                device.name,
                "update",
                timeout,
                key_path=control_client.key_path_for(device.name, self.control_key_dir),
            )
        sock = thread_socket.udp_socket(device.address, timeout)
        try:
            sock.sendto(b"update", (device.address, COMMAND_PORT))
            return sock.recv(512).decode(errors="replace")
        except (OSError, socket.timeout):
            return None
        finally:
            sock.close()


def smp_python_from(smpmgr: pathlib.Path) -> pathlib.Path:
    try:
        first = smpmgr.open().readline().strip()
    except OSError as exc:
        raise ValueError(f"cannot read smpmgr launcher {smpmgr}: {exc}") from exc
    if not first.startswith("#!"):
        raise ValueError(f"smpmgr launcher {smpmgr} has no interpreter shebang")
    interpreter = pathlib.Path(first[2:].split()[0])
    if not interpreter.is_file() or not os.access(interpreter, os.X_OK):
        raise ValueError(f"smpmgr interpreter is not executable: {interpreter}")
    return interpreter


class DeploymentFailure(RuntimeError):
    def __init__(self, classification: str, message: str, *, ambiguous: bool = False):
        super().__init__(message)
        self.classification = classification
        self.ambiguous = ambiguous


@dataclasses.dataclass(frozen=True)
class DeployConfig:
    command_timeout: float = 0.5
    state_timeout: float = 5.0
    upload_timeout: float = 360.0
    upload_attempts: int = 3
    upload_retry_delay: float = 5.0
    upload_verify_timeout: float = 45.0
    reboot_timeout: float = 180.0
    poll_interval: float = 1.0


def upload_and_verify(
    device: Device,
    image: ImageMetadata,
    backend: Backend,
    events: EventLog,
    config: DeployConfig,
    *,
    sleep: Callable[[float], None] = time.sleep,
    monotonic: Callable[[], float] = time.monotonic,
) -> tuple[dict[str, object], list[dict[str, object]]]:
    """Upload an image and prove the exact inactive slot before returning."""
    upload_errors: list[str] = []
    for attempt in range(1, config.upload_attempts + 1):
        events.emit("upload_started", device=device.name, attempt=attempt, size=image.size)
        try:
            backend.upload(device, image.path, config.upload_timeout)
            events.emit("upload_process_complete", device=device.name, attempt=attempt)
            break
        except Exception as exc:
            upload_errors.append(str(exc))
            events.emit(
                "upload_attempt_failed", device=device.name, attempt=attempt, error=str(exc)
            )
            if attempt == config.upload_attempts:
                raise DeploymentFailure(
                    "upload_failed", f"all upload attempts failed: {upload_errors}"
                ) from exc
            sleep(config.upload_retry_delay)

    deadline = monotonic() + config.upload_verify_timeout
    candidate = None
    last_states: list[dict[str, object]] | None = None
    last_error: str | None = None
    while monotonic() < deadline:
        try:
            last_states = backend.states(device, config.state_timeout)
            candidate = verify_inactive_candidate(last_states, image)
            if candidate:
                break
        except Exception as exc:
            last_error = str(exc)
        sleep(config.poll_interval)
    if candidate is None or last_states is None:
        raise DeploymentFailure(
            "stale_slot",
            f"inactive slot never matched exact candidate; states={last_states}, error={last_error}",
        )
    events.emit("upload_verified", device=device.name, candidate=candidate, states=last_states)
    return candidate, last_states


def stage_transaction(
    device: Device,
    image: ImageMetadata,
    backend: Backend,
    events: EventLog,
    config: DeployConfig,
    *,
    allow_downgrade: bool = False,
    allow_same_version: bool = False,
    sleep: Callable[[float], None] = time.sleep,
    monotonic: Callable[[], float] = time.monotonic,
) -> dict[str, object]:
    """Stage and verify an inactive image without arming or rebooting it."""
    status = backend.status(device, config.command_timeout)
    try:
        states = backend.states(device, config.state_timeout)
    except Exception as exc:
        raise DeploymentFailure("state_unreachable", f"preflight state read failed: {exc}") from exc
    baseline = validate_preflight(
        device,
        image,
        status,
        states,
        allow_downgrade=allow_downgrade,
        allow_same_version=allow_same_version,
    )
    events.emit("preflight_passed", device=device.name, status=status, states=states)
    if state_matches(baseline, image):
        raise DeploymentFailure(
            "candidate_active", "refusing to stage an image that is already active"
        )

    candidate = verify_inactive_candidate(states, image)
    classification = "already_staged"
    if candidate is None:
        candidate, _ = upload_and_verify(
            device,
            image,
            backend,
            events,
            config,
            sleep=sleep,
            monotonic=monotonic,
        )
        classification = "staged"

    # Re-read identity, baseline, and candidate immediately before accepting
    # the stage. This command deliberately never invokes backend.update().
    status = backend.status(device, config.command_timeout)
    states = backend.states(device, config.state_timeout)
    current = active_state(states)
    candidate = verify_inactive_candidate(states, image)
    if not status or status.get("name") != device.name or status.get("infra") != 1:
        raise DeploymentFailure("stage_preflight_failed", "identity/reachability changed")
    if str(current.get("hash", "")).upper() != str(baseline.get("hash", "")).upper():
        raise DeploymentFailure("stage_preflight_failed", "active image changed while staging")
    if candidate is None:
        raise DeploymentFailure("stage_preflight_failed", "exact inactive candidate changed")
    result = {
        "classification": classification,
        "device": device.name,
        "image": image.as_dict(),
        "active": current,
        "candidate": candidate,
        "status": status,
    }
    events.emit("stage_succeeded", **result)
    return result


def active_state(states: list[dict[str, object]]) -> dict[str, object]:
    active = [state for state in states if state.get("active") is True]
    if len(active) != 1:
        raise DeploymentFailure("invalid_slot_state", f"expected one active image, got {active}")
    return active[0]


def state_matches(state: dict[str, object], image: ImageMetadata) -> bool:
    try:
        version = Version.parse(str(state.get("version")))
    except ValueError:
        return False
    return (
        str(state.get("hash", "")).upper() == image.image_hash
        and version.tuple() == image.version.tuple()
    )


def validate_preflight(
    device: Device,
    image: ImageMetadata,
    status: dict[str, object] | None,
    states: list[dict[str, object]],
    *,
    allow_downgrade: bool,
    allow_same_version: bool,
) -> dict[str, object]:
    if not status or status.get("name") != device.name:
        raise DeploymentFailure("identity_failed", "direct status identity did not match inventory")
    if status.get("infra") != 1:
        raise DeploymentFailure("reachability_failed", "target does not report infra=1")
    active = active_state(states)
    if active.get("confirmed") is not True:
        raise DeploymentFailure("active_unconfirmed", f"active image is not confirmed: {active}")
    if any(state.get("pending") is True for state in states):
        raise DeploymentFailure("pending_image", "an image is already pending")
    try:
        current = Version.parse(str(active.get("version")))
    except ValueError as exc:
        raise DeploymentFailure("invalid_slot_state", str(exc)) from exc
    if state_matches(active, image):
        return active
    if image.version.tuple() < current.tuple() and not allow_downgrade:
        raise DeploymentFailure(
            "version_policy",
            f"candidate {image.version} is older than active {current}; use --allow-downgrade",
        )
    if image.version.tuple() == current.tuple() and not allow_same_version:
        raise DeploymentFailure(
            "version_policy",
            f"candidate {image.version} equals active {current}; use --allow-same-version",
        )
    return active


def verify_inactive_candidate(
    states: list[dict[str, object]], image: ImageMetadata
) -> dict[str, object] | None:
    matches = [
        state for state in states if state.get("active") is not True and state_matches(state, image)
    ]
    if len(matches) != 1:
        return None
    candidate = matches[0]
    if candidate.get("pending") is True or candidate.get("confirmed") is True:
        return None
    return candidate


def deploy_transaction(
    device: Device,
    image: ImageMetadata,
    backend: Backend,
    events: EventLog,
    config: DeployConfig,
    *,
    allow_downgrade: bool = False,
    allow_same_version: bool = False,
    sleep: Callable[[float], None] = time.sleep,
    monotonic: Callable[[], float] = time.monotonic,
) -> dict[str, object]:
    status = backend.status(device, config.command_timeout)
    try:
        states = backend.states(device, config.state_timeout)
    except Exception as exc:
        raise DeploymentFailure("state_unreachable", f"preflight state read failed: {exc}") from exc
    baseline = validate_preflight(
        device,
        image,
        status,
        states,
        allow_downgrade=allow_downgrade,
        allow_same_version=allow_same_version,
    )
    events.emit("preflight_passed", device=device.name, status=status, states=states)
    if state_matches(baseline, image):
        result = {
            "classification": "already_current",
            "device": device.name,
            "image": image.as_dict(),
            "active": baseline,
            "status": status,
        }
        events.emit("deployment_skipped", **result)
        return result

    candidate = verify_inactive_candidate(states, image)
    if candidate is None:
        candidate, last_states = upload_and_verify(
            device,
            image,
            backend,
            events,
            config,
            sleep=sleep,
            monotonic=monotonic,
        )
    else:
        last_states = states
        events.emit(
            "upload_skipped",
            device=device.name,
            reason="exact_candidate_already_staged",
            candidate=candidate,
        )

    # Close the time-of-check/time-of-arm window as much as the firmware API
    # permits: identity and exact inactive hash are both re-read immediately
    # before the self-arming command.
    status = backend.status(device, config.command_timeout)
    states = backend.states(device, config.state_timeout)
    current = active_state(states)
    candidate = verify_inactive_candidate(states, image)
    if not status or status.get("name") != device.name or status.get("infra") != 1:
        raise DeploymentFailure("arm_preflight_failed", "identity/reachability changed before arm")
    if str(current.get("hash", "")).upper() != str(baseline.get("hash", "")).upper():
        raise DeploymentFailure("arm_preflight_failed", "active image changed before arm")
    if candidate is None:
        raise DeploymentFailure(
            "arm_preflight_failed", "exact inactive candidate changed before arm"
        )
    events.emit("arm_preflight_passed", device=device.name, candidate=candidate)

    arm_acknowledged = True
    try:
        reply = backend.update(device, config.command_timeout)
    except control_client.ControlAuthReplyMissing as exc:
        # A self-rebooting command can tear down the radio before its signed
        # acknowledgement reaches the host. Do not resend it: reconcile the
        # exact signed image state across the observed reboot below.
        reply = None
        arm_acknowledged = False
        events.emit("arm_acknowledgement_lost", device=device.name, error=str(exc))
    else:
        events.emit("arm_command", device=device.name, reply=reply)
        if reply is None:
            arm_acknowledged = False
            events.emit("arm_acknowledgement_lost", device=device.name, error="no reply")
    if reply is not None and "armed, rebooting" not in reply:
        raise DeploymentFailure(
            "arm_ambiguous",
            f"update acknowledgement missing or invalid: {reply!r}",
            ambiguous=True,
        )

    started = monotonic()
    deadline = started + config.reboot_timeout
    saw_down = False
    saw_candidate_active = False
    saw_candidate_unconfirmed = False
    saw_baseline_return = False
    last_status = None
    last_states = None
    while monotonic() < deadline:
        status = backend.status(device, config.command_timeout)
        last_status = status
        if status is None:
            if not saw_down:
                events.emit("device_left", device=device.name, elapsed=monotonic() - started)
            saw_down = True
            sleep(config.poll_interval)
            continue
        if status.get("name") != device.name:
            raise DeploymentFailure(
                "identity_changed",
                f"unexpected status identity after arm: {status}",
                ambiguous=True,
            )
        try:
            states = backend.states(device, config.state_timeout)
            last_states = states
        except Exception as exc:
            events.emit("state_poll_failed", device=device.name, error=str(exc))
            sleep(config.poll_interval)
            continue
        active = active_state(states)
        if state_matches(active, image):
            saw_candidate_active = True
            if active.get("confirmed") is True and status.get("infra") == 1:
                if not saw_down:
                    raise DeploymentFailure(
                        "reboot_not_observed",
                        "candidate is active and confirmed but no departure was observed",
                        ambiguous=True,
                    )
                result = {
                    "classification": "success",
                    "device": device.name,
                    "duration": monotonic() - started,
                    "image": image.as_dict(),
                    "active": active,
                    "status": status,
                    "saw_down": True,
                    "arm_acknowledged": arm_acknowledged,
                }
                events.emit("deployment_succeeded", **result)
                return result
            if active.get("confirmed") is not True:
                if not saw_candidate_unconfirmed:
                    events.emit(
                        "candidate_active_unconfirmed",
                        device=device.name,
                        elapsed=monotonic() - started,
                        active=active,
                    )
                saw_candidate_unconfirmed = True
        elif (
            saw_down
            and str(active.get("hash", "")).upper() == str(baseline.get("hash", "")).upper()
            and active.get("confirmed") is True
            and status.get("infra") == 1
        ):
            saw_baseline_return = True
            break
        sleep(config.poll_interval)

    if saw_baseline_return:
        classification = "rollback"
    elif saw_candidate_unconfirmed:
        classification = "confirmation_timeout"
    elif saw_down and last_status is None:
        classification = "reboot_loss"
    elif saw_candidate_active:
        classification = "confirmation_timeout"
    else:
        classification = "verification_timeout"
    raise DeploymentFailure(
        classification,
        f"deployment did not confirm; status={last_status}, states={last_states}",
        ambiguous=classification in {"reboot_loss", "verification_timeout"},
    )


@contextlib.contextmanager
def device_lock(device: Device, lock_dir: pathlib.Path) -> Iterator[None]:
    lock_dir.mkdir(parents=True, exist_ok=True)
    slug = re.sub(r"[^A-Za-z0-9_.-]", "_", device.name)
    path = lock_dir / f"thread-ring-{slug}.lock"
    handle = path.open("w")
    try:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise DeploymentFailure("device_locked", f"another deployment holds {path}") from exc
        handle.write(json.dumps({"pid": os.getpid(), "device": device.name, "wall": utc_now()}))
        handle.flush()
        yield
    finally:
        handle.close()


def default_outdir(version: Version) -> pathlib.Path:
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    return pathlib.Path.home() / "deployments" / f"{stamp}-v{version}"


def config_from_args(args: argparse.Namespace) -> DeployConfig:
    return DeployConfig(
        command_timeout=args.command_timeout,
        state_timeout=args.state_timeout,
        upload_timeout=args.upload_timeout,
        upload_attempts=args.upload_attempts,
        upload_retry_delay=args.upload_retry_delay,
        upload_verify_timeout=args.upload_verify_timeout,
        reboot_timeout=args.reboot_timeout,
        poll_interval=args.poll_interval,
    )


def add_deploy_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--inventory", type=pathlib.Path, default=DEFAULT_INVENTORY)
    parser.add_argument("--out", type=pathlib.Path)
    parser.add_argument(
        "--smpmgr", type=pathlib.Path, default=pathlib.Path.home() / ".local/bin/smpmgr"
    )
    parser.add_argument("--smp-python", type=pathlib.Path)
    parser.add_argument(
        "--smp-mtu",
        type=int,
        default=DEFAULT_SMP_MTU,
        help=("SMP UDP MTU; 192 is the fleet default to limit 802.15.4 fragment loss"),
    )
    parser.add_argument(
        "--smp-chunk-attempts",
        type=int,
        default=DEFAULT_SMP_CHUNK_ATTEMPTS,
        help="same-packet retries before reconnecting (default: 3)",
    )
    parser.add_argument(
        "--smp-upload-request-timeout",
        type=float,
        default=DEFAULT_SMP_UPLOAD_REQUEST_TIMEOUT,
        help="per-packet upload timeout in seconds (default: 3)",
    )
    parser.add_argument(
        "--smp-helper",
        type=pathlib.Path,
        default=pathlib.Path(__file__).with_name("smp_transport.py"),
    )
    parser.add_argument(
        "--lock-dir", type=pathlib.Path, default=pathlib.Path("/tmp/thread-ring-deploy-locks")
    )
    parser.add_argument("--control-key-dir", type=pathlib.Path)
    parser.add_argument(
        "--allow-legacy-control",
        action="store_true",
        help="explicit migration-only override for pre-v2 firmware",
    )
    parser.add_argument("--allow-downgrade", action="store_true")
    parser.add_argument("--allow-same-version", action="store_true")
    parser.add_argument("--command-timeout", type=float, default=0.5)
    parser.add_argument("--state-timeout", type=float, default=5.0)
    parser.add_argument("--upload-timeout", type=float, default=360.0)
    parser.add_argument(
        "--upload-attempts",
        type=int,
        default=8,
        help="bounded resumable upload processes (default: 8)",
    )
    parser.add_argument("--upload-retry-delay", type=float, default=5.0)
    parser.add_argument("--upload-verify-timeout", type=float, default=45.0)
    parser.add_argument("--reboot-timeout", type=float, default=180.0)
    parser.add_argument("--poll-interval", type=float, default=1.0)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    inspect_parser = sub.add_parser("inspect-image")
    inspect_parser.add_argument("image", type=pathlib.Path)
    deploy_parser = sub.add_parser("deploy")
    deploy_parser.add_argument("target")
    deploy_parser.add_argument("image", type=pathlib.Path)
    add_deploy_options(deploy_parser)
    stage_parser = sub.add_parser("stage")
    stage_parser.add_argument("target")
    stage_parser.add_argument("image", type=pathlib.Path)
    add_deploy_options(stage_parser)
    fleet_parser = sub.add_parser("fleet")
    fleet_parser.add_argument("image", type=pathlib.Path)
    fleet_parser.add_argument("--targets", nargs="+")
    add_deploy_options(fleet_parser)
    args = parser.parse_args(argv)
    for field in (
        "command_timeout",
        "state_timeout",
        "upload_timeout",
        "upload_retry_delay",
        "upload_verify_timeout",
        "reboot_timeout",
        "poll_interval",
        "smp_upload_request_timeout",
    ):
        if hasattr(args, field) and getattr(args, field) <= 0:
            parser.error(f"--{field.replace('_', '-')} must be positive")
    if hasattr(args, "upload_attempts") and not 1 <= args.upload_attempts <= 12:
        parser.error("--upload-attempts must be between 1 and 12")
    if hasattr(args, "smp_mtu") and not MIN_SMP_MTU <= args.smp_mtu <= MAX_SMP_MTU:
        parser.error(f"--smp-mtu must be between {MIN_SMP_MTU} and {MAX_SMP_MTU}")
    if (
        hasattr(args, "smp_chunk_attempts")
        and not 1 <= args.smp_chunk_attempts <= MAX_SMP_CHUNK_ATTEMPTS
    ):
        parser.error(f"--smp-chunk-attempts must be between 1 and {MAX_SMP_CHUNK_ATTEMPTS}")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        image = inspect_image(args.image)
    except ValueError as exc:
        print(f"deploy: {exc}", file=sys.stderr)
        return 2
    if args.action == "inspect-image":
        print(json.dumps(image.as_dict(), indent=2, sort_keys=True))
        return 0

    try:
        inventory = load_inventory(args.inventory)
        by_name = {device.name: device for device in inventory}
        if args.action in {"deploy", "stage"}:
            if args.target not in by_name:
                raise ValueError(f"target {args.target!r} is not in inventory")
            selected = [by_name[args.target]]
        else:
            selected = inventory if not args.targets else [by_name[name] for name in args.targets]
            selected = sorted(selected, key=lambda device: (device.rollout_order, device.name))
            if not selected or not selected[0].canary:
                raise ValueError("fleet rollout must include the canary first")
        smp_python = args.smp_python or smp_python_from(args.smpmgr)
        if not args.smp_helper.is_file():
            raise ValueError(f"SMP helper not found: {args.smp_helper}")
    except (ValueError, KeyError) as exc:
        print(f"deploy: {exc}", file=sys.stderr)
        return 2

    outdir = args.out or default_outdir(image.version)
    events = EventLog(outdir)
    backend = SubprocessBackend(
        smp_python,
        args.smp_helper,
        mtu=args.smp_mtu,
        chunk_attempts=args.smp_chunk_attempts,
        upload_request_timeout=args.smp_upload_request_timeout,
        control_key_dir=args.control_key_dir,
        allow_legacy_control=args.allow_legacy_control,
    )
    results: list[dict[str, object]] = []
    exit_code = 0
    try:
        events.write_json(
            "invocation.json",
            {
                **{
                    key: str(value) if isinstance(value, pathlib.Path) else value
                    for key, value in vars(args).items()
                },
                "image_metadata": image.as_dict(),
                "devices": [dataclasses.asdict(device) for device in selected],
            },
        )
        for device in selected:
            events.emit("deployment_started", device=device.name, address=device.address)
            try:
                with device_lock(device, args.lock_dir):
                    transaction = (
                        stage_transaction if args.action == "stage" else deploy_transaction
                    )
                    result = transaction(
                        device,
                        image,
                        backend,
                        events,
                        config_from_args(args),
                        allow_downgrade=args.allow_downgrade,
                        allow_same_version=args.allow_same_version,
                    )
                results.append(result)
            except DeploymentFailure as exc:
                result = {
                    "classification": exc.classification,
                    "device": device.name,
                    "error": str(exc),
                    "ambiguous": exc.ambiguous,
                }
                results.append(result)
                events.emit("deployment_failed", **result)
                events.emit("rollout_stopped", device=device.name, reason=exc.classification)
                exit_code = 3 if exc.ambiguous else 1
                break
            except Exception as exc:
                result = {
                    "classification": "internal_error",
                    "device": device.name,
                    "error": f"{type(exc).__name__}: {exc}",
                    "ambiguous": True,
                }
                results.append(result)
                events.emit("deployment_failed", **result)
                exit_code = 3
                break
    finally:
        summary = {"exit_code": exit_code, "results": results}
        events.write_json("result.json", summary)
        events.close()
    print(json.dumps(summary, indent=2, sort_keys=True))
    print(f"evidence: {outdir}")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
