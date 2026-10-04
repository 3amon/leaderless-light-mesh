#!/usr/bin/env python3
"""Authenticated, replay-safe UDP control client for mutating ring commands."""

from __future__ import annotations

import contextlib
import fcntl
import hashlib
import hmac
import os
import pathlib
import re
import socket
import stat

import thread_socket

COMMAND_PORT = 4242
MAX_DATAGRAM = 96
TAG_BYTES = 12
KEY_BYTES = 32
MUTATING_COMMANDS = {
    "bright",
    "budget",
    "solo",
    "always-on",
    "flash",
    "gossip",
    "heal",
    "hb",
    "layout",
    "mode",
    "pattern",
    "power",
    "quiet",
    "reboot",
    "scramble",
    "skew",
    "sleep",
    "update",
}
AUTH_STATUS = re.compile(r"^auth=(ready|unprovisioned) seq=([0-9a-f]{16})$")
AUTH_RESPONSE = re.compile(r"^(v2 ([0-9a-f]{16}) (ok|error) (\S(?:.*\S)?)) ([0-9a-f]{24})$")
DEVICE_NAME = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")


class ControlAuthError(RuntimeError):
    """The authenticated control boundary could not safely authorize a command."""


class ControlAuthReplyMissing(ControlAuthError):
    """The authenticated mutation was sent but its response was not received."""


def default_key_dir() -> pathlib.Path:
    return pathlib.Path(
        os.environ.get("THREAD_LED_CONTROL_KEY_DIR", "/etc/thread-led-node/control-keys")
    )


def default_lock_dir() -> pathlib.Path:
    configured = os.environ.get("THREAD_LED_CONTROL_LOCK_DIR")
    if configured:
        return pathlib.Path(configured)
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    if runtime:
        return pathlib.Path(runtime) / "thread-led-node" / "control-locks"
    return pathlib.Path.home() / ".cache" / "thread-led-node" / "control-locks"


def key_path_for(device_name: str, key_dir: pathlib.Path | None = None) -> pathlib.Path:
    if not DEVICE_NAME.fullmatch(device_name):
        raise ControlAuthError(f"invalid inventory device name: {device_name!r}")
    return (key_dir or default_key_dir()) / f"{device_name}.key"


def load_key(path: pathlib.Path) -> bytes:
    try:
        metadata = path.lstat()
    except OSError as exc:
        raise ControlAuthError(f"cannot stat control key {path}: {exc}") from exc
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise ControlAuthError(f"control key is not a regular non-symlink file: {path}")
    if metadata.st_uid != os.geteuid():
        raise ControlAuthError(f"control key is not owned by uid {os.geteuid()}: {path}")
    if stat.S_IMODE(metadata.st_mode) & 0o077:
        raise ControlAuthError(f"control key must not grant group/other access: {path}")
    try:
        encoded = path.read_text(encoding="ascii").strip()
        key = bytes.fromhex(encoded)
    except (OSError, UnicodeError, ValueError) as exc:
        raise ControlAuthError(f"cannot read 64-hex control key {path}: {exc}") from exc
    if len(encoded) != KEY_BYTES * 2 or encoded.lower() != encoded or len(key) != KEY_BYTES:
        raise ControlAuthError(f"control key must be exactly 64 lowercase hex characters: {path}")
    return key


def parse_auth_status(reply: str | None) -> tuple[bool, int]:
    match = AUTH_STATUS.fullmatch(reply or "")
    if not match:
        raise ControlAuthError(f"device returned invalid authentication status: {reply!r}")
    return match.group(1) == "ready", int(match.group(2), 16)


def validate_mutation(command: str) -> None:
    try:
        encoded = command.encode("ascii")
    except UnicodeEncodeError as exc:
        raise ControlAuthError("control command must be ASCII") from exc
    if not encoded or command.strip() != command or "  " in command:
        raise ControlAuthError("control command must use canonical single-space tokens")
    if any(byte < 0x20 or byte > 0x7E for byte in encoded):
        raise ControlAuthError("control command contains non-printable text")
    if not command_is_mutating(command):
        raise ControlAuthError("authenticated client accepts mutating commands only")


def command_is_mutating(command: str) -> bool:
    name = command.split(" ", 1)[0]
    return name in MUTATING_COMMANDS and command not in {"layout", "mode", "power", "quiet"}


def build_authenticated(command: str, sequence: int, key: bytes) -> bytes:
    validate_mutation(command)
    if len(key) != KEY_BYTES:
        raise ControlAuthError("control key must be exactly 32 bytes")
    if sequence <= 0 or sequence > 0xFFFFFFFFFFFFFFFF:
        raise ControlAuthError("control sequence must be between 1 and 2^64-1")
    prefix = f"v2 {sequence:016x} {command}".encode("ascii")
    tag = hmac.new(key, prefix, hashlib.sha256).digest()[:TAG_BYTES].hex().encode("ascii")
    datagram = prefix + b" " + tag
    if len(datagram) > MAX_DATAGRAM:
        raise ControlAuthError(f"authenticated command exceeds {MAX_DATAGRAM}-byte datagram limit")
    return datagram


def parse_authenticated_response(reply: str | None, sequence: int, key: bytes) -> str:
    if reply is None:
        raise ControlAuthReplyMissing("device did not return an authenticated response")
    match = AUTH_RESPONSE.fullmatch(reply or "")
    if not match:
        raise ControlAuthError(f"device returned invalid authenticated response: {reply!r}")
    response_sequence = int(match.group(2), 16)
    if response_sequence != sequence:
        raise ControlAuthError(
            f"response sequence mismatch: expected {sequence:016x}, got {response_sequence:016x}"
        )
    expected_tag = hmac.new(key, match.group(1).encode("ascii"), hashlib.sha256).digest()[
        :TAG_BYTES
    ]
    try:
        received_tag = bytes.fromhex(match.group(5))
    except ValueError as exc:
        raise ControlAuthError("device returned malformed response authentication tag") from exc
    if not hmac.compare_digest(expected_tag, received_tag):
        raise ControlAuthError("device response authentication failed")
    payload = match.group(4)
    if match.group(3) == "error":
        raise ControlAuthError(f"device rejected authenticated command: {payload}")
    return payload


def udp_exchange(address: str, datagram: bytes, timeout: float) -> str | None:
    sock = thread_socket.udp_socket(address, timeout)
    try:
        sock.sendto(datagram, (address, COMMAND_PORT))
        return sock.recv(512).decode(errors="replace")
    except (OSError, socket.timeout):
        return None
    finally:
        sock.close()


@contextlib.contextmanager
def device_lock(device_name: str, lock_dir: pathlib.Path | None = None):
    root = lock_dir or default_lock_dir()
    root.mkdir(mode=0o700, parents=True, exist_ok=True)
    path = root / f"{device_name}.lock"
    descriptor = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        yield
    finally:
        fcntl.flock(descriptor, fcntl.LOCK_UN)
        os.close(descriptor)


def authenticated_udp_command(
    address: str,
    device_name: str,
    command: str,
    timeout: float,
    *,
    key_path: pathlib.Path | None = None,
    lock_dir: pathlib.Path | None = None,
    exchange=udp_exchange,
) -> str:
    key = load_key(key_path or key_path_for(device_name))
    with device_lock(device_name, lock_dir):
        ready, remote_sequence = parse_auth_status(exchange(address, b"auth", timeout))
        if not ready:
            raise ControlAuthError(f"device {device_name!r} has no provisioned control key")
        if remote_sequence == 0xFFFFFFFFFFFFFFFF:
            raise ControlAuthError(f"device {device_name!r} exhausted its control sequence")
        sequence = remote_sequence + 1
        datagram = build_authenticated(command, sequence, key)
        reply = exchange(address, datagram, timeout)
        return parse_authenticated_response(reply, sequence, key)
