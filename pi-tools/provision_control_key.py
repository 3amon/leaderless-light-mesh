#!/usr/bin/env python3
"""Provision and verify one inventoried ring's UDP control key over USB."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import pathlib
import select
import stat
import subprocess
import sys
import termios
import time

import control_client

import boot_test
import deploy

DEFAULT_INVENTORY = pathlib.Path(__file__).resolve().parents[1] / "config/rings.json"


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


def inventory_target(path: pathlib.Path, name: str) -> deploy.Device:
    matches = [device for device in deploy.load_inventory(path) if device.name == name]
    if len(matches) != 1:
        raise ValueError(f"target {name!r} does not resolve to exactly one inventory record")
    target = matches[0]
    if not target.usb_serial:
        raise ValueError(f"target {name!r} has no inventoried USB serial")
    return target


def _validate_key_directory(path: pathlib.Path) -> None:
    metadata = path.lstat()
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
        raise control_client.ControlAuthError(
            f"control key directory is not a non-symlink directory: {path}"
        )
    if metadata.st_uid != os.geteuid():
        raise control_client.ControlAuthError(
            f"control key directory is not owned by uid {os.geteuid()}: {path}"
        )
    if stat.S_IMODE(metadata.st_mode) & 0o077:
        raise control_client.ControlAuthError(
            f"control key directory must not grant group/other access: {path}"
        )


def create_or_load_key(path: pathlib.Path) -> tuple[bytes, bool]:
    try:
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        _validate_key_directory(path.parent)
    except OSError as exc:
        raise control_client.ControlAuthError(
            f"cannot create private control key directory {path.parent}: {exc}"
        ) from exc

    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags, 0o600)
    except FileExistsError:
        return control_client.load_key(path), False
    except OSError as exc:
        raise control_client.ControlAuthError(f"cannot create control key {path}: {exc}") from exc

    key = os.urandom(control_client.KEY_BYTES)
    encoded = key.hex().encode("ascii") + b"\n"
    try:
        offset = 0
        while offset < len(encoded):
            offset += os.write(descriptor, encoded[offset:])
        os.fsync(descriptor)
    except Exception:
        os.close(descriptor)
        path.unlink(missing_ok=True)
        raise
    os.close(descriptor)
    return key, True


def shell_prompt_seen(captured: bytes | bytearray) -> bool:
    """Recognize Zephyr's prompt without depending on ANSI color escapes."""
    return b":~$" in captured


def write_paced(descriptor: int, payload: bytes | bytearray) -> None:
    """Stay below the provisioning shell's deliberately small USB RX ring."""
    offset = 0
    chunk_size = 8
    while offset < len(payload):
        chunk_end = min(offset + chunk_size, len(payload))
        while offset < chunk_end:
            offset += os.write(descriptor, payload[offset:chunk_end])
        if offset < len(payload):
            time.sleep(0.02)


def provision_over_serial(usb_serial: str, key: bytes, timeout: float = 5.0) -> None:
    path = boot_test.usb_path(usb_serial)
    if path is None:
        raise RuntimeError(f"inventoried USB serial {usb_serial!r} is not attached")
    configured = subprocess.run(
        ["stty", "-F", path, "115200", "raw", "-echo"],
        text=True,
        capture_output=True,
        check=False,
        timeout=5,
    )
    if configured.returncode:
        raise RuntimeError("could not configure the inventoried USB shell")

    descriptor = os.open(path, os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)
    captured = bytearray()
    command = bytearray()
    try:
        termios.tcflush(descriptor, termios.TCIOFLUSH)
        deadline = time.monotonic() + timeout
        os.write(descriptor, b"\r\n")
        while time.monotonic() < deadline:
            readable, _, _ = select.select(
                [descriptor], [], [], min(0.2, max(0.0, deadline - time.monotonic()))
            )
            if not readable:
                continue
            try:
                captured.extend(os.read(descriptor, 4096))
            except BlockingIOError:
                continue
            if shell_prompt_seen(captured):
                break
            if len(captured) > 8192:
                del captured[:-4096]
        else:
            raise RuntimeError("USB shell did not become ready for control-key provisioning")

        captured[:] = b"\x00" * len(captured)
        captured.clear()
        command.extend(b"ring auth-key " + key.hex().encode("ascii") + b"\r\n")
        write_paced(descriptor, command)
        # The confirmation wait gets its own budget: a slow prompt above must not
        # starve it. Sharing one deadline across both phases made provisioning
        # fail with "did not confirm" whenever the shell was slow to come up.
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            readable, _, _ = select.select(
                [descriptor], [], [], min(0.2, max(0.0, deadline - time.monotonic()))
            )
            if not readable:
                continue
            try:
                chunk = os.read(descriptor, 4096)
            except BlockingIOError:
                continue
            captured.extend(chunk)
            if b"control key provisioned; sequence reset" in captured:
                return
            if len(captured) > 8192:
                del captured[:-4096]
        raise RuntimeError("USB shell did not confirm control-key provisioning")
    finally:
        for buffer in (command, captured):
            buffer[:] = b"\x00" * len(buffer)
        os.close(descriptor)


def query_auth(
    address: str, timeout: float, exchange=control_client.udp_exchange
) -> tuple[bool, int]:
    return control_client.parse_auth_status(exchange(address, b"auth", timeout))


def wait_until_ready(
    address: str,
    timeout: float,
    *,
    exchange=control_client.udp_exchange,
) -> int:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            ready, sequence = query_auth(address, min(0.5, timeout), exchange)
            if ready:
                return sequence
        except control_client.ControlAuthError as exc:
            last_error = exc
        time.sleep(0.2)
    if last_error:
        raise RuntimeError("device never returned valid ready authentication state") from last_error
    raise RuntimeError("device did not become control-auth ready")


def provision_target(
    target: deploy.Device,
    key_path: pathlib.Path,
    timeout: float,
    *,
    exchange=control_client.udp_exchange,
    serial_provision=provision_over_serial,
) -> dict[str, object]:
    key, created = create_or_load_key(key_path)
    ready, initial_sequence = query_auth(target.address, timeout, exchange)
    serial_written = False
    if not ready:
        if initial_sequence != 0:
            raise RuntimeError("unprovisioned device reported a nonzero control sequence")
        serial_provision(str(target.usb_serial), key, timeout)
        serial_written = True
        initial_sequence = wait_until_ready(target.address, timeout, exchange=exchange)

    response = control_client.authenticated_udp_command(
        target.address,
        target.name,
        "hb",
        timeout,
        key_path=key_path,
        exchange=exchange,
    )
    final_ready, final_sequence = query_auth(target.address, timeout, exchange)
    if not final_ready or final_sequence != initial_sequence + 1:
        raise RuntimeError("authenticated verification did not advance the device sequence once")
    return {
        "schema": 1,
        "time": utc_now(),
        "target": target.name,
        "address": target.address,
        "usb_serial": target.usb_serial,
        "key_created": created,
        "key_sha256": hashlib.sha256(key).hexdigest(),
        "serial_written": serial_written,
        "verification_command": "hb",
        "verification_response": response,
        "final_sequence": f"{final_sequence:016x}",
        "success": True,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True)
    parser.add_argument("--inventory", type=pathlib.Path, default=DEFAULT_INVENTORY)
    parser.add_argument("--key-dir", type=pathlib.Path, default=control_client.default_key_dir())
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--out", type=pathlib.Path)
    args = parser.parse_args()
    if args.timeout <= 0 or args.timeout > 120:
        parser.error("--timeout must be greater than zero and no more than 120 seconds")
    return args


def main() -> int:
    args = parse_args()
    try:
        target = inventory_target(args.inventory, args.target)
        key_path = control_client.key_path_for(target.name, args.key_dir)
        result = provision_target(target, key_path, args.timeout)
        output = json.dumps(result, indent=2, sort_keys=True) + "\n"
        if args.out:
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(output)
        print(output, end="")
        return 0
    except Exception as exc:
        print(f"provision_control_key: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
