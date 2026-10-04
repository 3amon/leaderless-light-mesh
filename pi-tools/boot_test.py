#!/usr/bin/env python3
"""Structured boot/reboot evidence harness for one Thread LED ring.

Run on the border-router Pi. The default action is observation only. Reboots
require the expected canary name and exact Thread address. Local canaries also
require their USB serial unless explicit network-only mode is selected.
"""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import glob
import json
import os
import pathlib
import re
import socket
import subprocess
import sys
import time
import urllib.request

import control_client
import thread_socket

CMD_PORT = 4242
DASHBOARD = "http://localhost:8080/api/state"
DEFAULT_CANARY = "opal-reef-tide"
DEFAULT_USB_SERIAL = "DEC0DE0000000022"
DEFAULT_CANARY_ADDRESS = "fd00:db8:0:1:a022:b022:c022:d022"


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


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


def parse_boot_status(reply: str | None) -> dict[str, object] | None:
    """Parse the firmware's persistent boot-stage ring without discarding raw evidence."""
    if not reply:
        return None
    parsed: dict[str, object] = {"raw": reply.strip(), "events": []}
    header = re.fullmatch(
        r"boot=(\d+) stage=([a-z-]+)(?: mcuboot=([0-9a-f]{2})/([0-9a-f]{2}))?"
        r"(?: ladder=(\d+))?"
        r" trail=(.*)",
        reply.strip(),
    )
    if not header:
        parsed["parse_error"] = "unrecognized boot status"
        return parsed
    parsed["boot"] = int(header.group(1))
    parsed["stage"] = header.group(2)
    if header.group(3) is not None:
        parsed["mcuboot_phase"] = int(header.group(3), 16)
        parsed["mcuboot_detail"] = int(header.group(4), 16)
    if header.group(5) is not None:
        parsed["escape_ladder_attempts"] = int(header.group(5))
    trail = header.group(6)
    if not trail:
        return parsed
    for item in trail.split(","):
        event = re.fullmatch(r"(\d+):([a-z-]+)@(\d+)", item)
        if not event:
            parsed["parse_error"] = f"unrecognized trail event: {item}"
            break
        parsed["events"].append(
            {
                "boot": int(event.group(1)),
                "stage": event.group(2),
                "uptime_ms": int(event.group(3)),
            }
        )
    return parsed


def dashboard_state(url: str = DASHBOARD) -> dict:
    with urllib.request.urlopen(url, timeout=3) as response:
        return json.load(response)


def resolve_node(name: str, state: dict) -> tuple[str, dict]:
    for key, node in (state.get("nodes") or {}).items():
        if node.get("name") == name:
            address = node.get("thread_addr") or key
            if address and ":" in str(address):
                return str(address), node
    raise RuntimeError(f"dashboard has no Thread node named {name!r}")


def udp_command(address: str, command: str, timeout: float) -> str | None:
    sock = thread_socket.udp_socket(address, timeout)
    try:
        sock.sendto(command.encode(), (address, CMD_PORT))
        return sock.recv(512).decode(errors="replace")
    except (OSError, socket.timeout):
        return None
    finally:
        sock.close()


def usb_path(serial: str | None) -> str | None:
    if serial is None:
        return None
    matches = sorted(glob.glob(f"/dev/serial/by-id/*{serial}*"))
    if len(matches) > 1:
        raise RuntimeError(f"USB serial {serial!r} matched multiple devices: {matches}")
    return os.path.realpath(matches[0]) if matches else None


def status_is_restarted(
    status: dict[str, object] | None,
    baseline_up: int,
    saw_usb_down: bool,
    saw_status_down: bool,
    usb_required: bool = True,
) -> bool:
    """Recognize a completed reboot even when recovery exceeds prior uptime."""
    if not status:
        return False
    uptime_rolled_back = int(status.get("up", baseline_up)) < baseline_up
    observed_full_outage = saw_status_down and (saw_usb_down or not usb_required)
    return uptime_rolled_back or observed_full_outage


def failure_domain(recovered: bool, saw_sentinel_down: bool) -> str | None:
    if recovered:
        return None
    return "infrastructure" if saw_sentinel_down else "target"


class Evidence:
    def __init__(self, outdir: pathlib.Path):
        self.outdir = outdir
        outdir.mkdir(parents=True, exist_ok=False)
        self.events = (outdir / "events.jsonl").open("w", buffering=1)
        self.start_monotonic = time.monotonic()

    def emit(self, kind: str, **fields) -> dict:
        event = {
            "t": time.monotonic() - self.start_monotonic,
            "monotonic_ns": time.monotonic_ns(),
            "wall": utc_now(),
            "event": kind,
            **fields,
        }
        self.events.write(json.dumps(event, sort_keys=True) + "\n")
        print(f"[{event['t']:8.3f}] {kind}: {fields}", flush=True)
        return event

    def write_json(self, name: str, value) -> None:
        (self.outdir / name).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")

    def close(self) -> None:
        self.events.close()


class SerialCapture:
    def __init__(self, serial: str | None, evidence: Evidence):
        self.serial = serial
        self.evidence = evidence
        self.fd: int | None = None
        self.path: str | None = None

    def poll(self) -> None:
        if self.serial is None:
            return
        path = usb_path(self.serial)
        if path != self.path:
            self.evidence.emit("usb", present=bool(path), path=path, serial=self.serial)
            self.path = path
        if path and self.fd is None:
            try:
                self.fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOCTTY)
                self.evidence.emit("serial_open", path=path)
            except OSError as exc:
                self.evidence.emit("serial_open_error", path=path, error=str(exc))
        if not path and self.fd is not None:
            self._close("usb_absent")
        if self.fd is not None:
            try:
                data = os.read(self.fd, 4096)
                if data:
                    self.evidence.emit(
                        "serial_data",
                        text=data.decode(errors="replace"),
                        base64=base64.b64encode(data).decode(),
                    )
            except BlockingIOError:
                pass
            except OSError as exc:
                self.evidence.emit("serial_read_error", error=str(exc))
                self._close("read_error")

    def _close(self, reason: str) -> None:
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.evidence.emit("serial_close", reason=reason)
            self.fd = None

    def close(self) -> None:
        self._close("harness_exit")


def kernel_log_since(wall_start: str) -> str:
    result = subprocess.run(
        ["journalctl", "-k", "--since", wall_start, "--no-pager", "-o", "short-iso-precise"],
        text=True,
        capture_output=True,
        check=False,
    )
    return result.stdout + result.stderr


def validate_target_health(
    target: str,
    expected_address: str,
    expected_img: str | None,
    address: str,
    node: dict,
    status: dict[str, object] | None,
    boot_status: dict[str, object] | None,
) -> None:
    if address.lower() != expected_address.lower():
        raise RuntimeError(
            f"identity mismatch: resolved address {address!r} != "
            f"expected address {expected_address!r}"
        )
    if not status or status.get("name") != target:
        raise RuntimeError("direct status identity did not match target")
    if status.get("infra") != 1:
        raise RuntimeError("target does not report infra=1")
    if not boot_status or boot_status.get("parse_error") or boot_status.get("stage") != "confirmed":
        raise RuntimeError("target boot trail is not parsed and confirmed")
    if expected_img and node.get("img") != expected_img:
        raise RuntimeError(f"dashboard image {node.get('img')!r} != {expected_img!r}")


def preflight(args, evidence: Evidence) -> tuple[str, dict, dict]:
    state = dashboard_state(args.dashboard)
    address, node = resolve_node(args.target, state)
    path = usb_path(args.usb_serial) if not args.network_only else None
    status = parse_status(udp_command(address, "status", args.command_timeout))
    boot_status = parse_boot_status(udp_command(address, "boot", args.command_timeout))
    evidence.write_json("dashboard-before.json", state)
    evidence.emit(
        "preflight",
        target=args.target,
        address=address,
        usb_serial=args.usb_serial,
        usb_path=path,
        dashboard_node=node,
        status=status,
        boot_status=boot_status,
    )
    strict_health = args.action == "reboot" or args.require_healthy
    if args.action == "reboot":
        if args.target != args.canary_name:
            raise RuntimeError(
                f"refusing reboot: target {args.target!r} is not canary {args.canary_name!r}"
            )
        if not args.network_only and not path:
            raise RuntimeError(f"refusing reboot: canary USB serial {args.usb_serial} is absent")
    if strict_health:
        validate_target_health(
            args.target,
            args.expected_address,
            args.expected_img,
            address,
            node,
            status,
            boot_status,
        )
        if args.sentinel_address:
            sentinel = parse_status(
                udp_command(args.sentinel_address, "status", args.command_timeout)
            )
            evidence.emit(
                "sentinel_preflight",
                name=args.sentinel_name,
                address=args.sentinel_address,
                status=sentinel,
            )
            if not sentinel or sentinel.get("name") != args.sentinel_name:
                raise RuntimeError("sentinel identity did not match")
            if sentinel.get("infra") != 1:
                raise RuntimeError("sentinel does not report infra=1")
    return address, node, status or {}


def observe(args, evidence: Evidence, capture: SerialCapture, address: str) -> None:
    deadline = time.monotonic() + args.observe_seconds
    last_status = object()
    while time.monotonic() < deadline:
        capture.poll()
        status = parse_status(udp_command(address, "status", args.command_timeout))
        comparable = status and status.get("raw")
        if comparable != last_status:
            evidence.emit("status", available=bool(status), status=status)
            last_status = comparable
        time.sleep(args.poll_interval)


def run_trial(args, evidence: Evidence, capture: SerialCapture, address: str, trial: int) -> dict:
    baseline = parse_status(udp_command(address, "status", args.command_timeout))
    baseline_boot = parse_boot_status(udp_command(address, "boot", args.command_timeout))
    baseline_up = int(baseline.get("up", -1)) if baseline else -1
    wall_start = utc_now()
    evidence.emit("trial_start", trial=trial, baseline=baseline, baseline_boot=baseline_boot)
    if args.allow_legacy_control:
        reply = udp_command(address, "reboot", args.command_timeout)
    else:
        reply = control_client.authenticated_udp_command(
            address,
            args.target,
            "reboot",
            args.command_timeout,
            key_path=control_client.key_path_for(args.target, args.control_key_dir),
        )
    evidence.emit("reboot_command", trial=trial, reply=reply)
    if not reply or "rebooting" not in reply:
        raise RuntimeError(f"trial {trial}: reboot command was not acknowledged")

    started = time.monotonic()
    deadline = started + args.max_recovery_seconds
    saw_usb_down = False
    saw_status_down = False
    usb_down_at = usb_up_at = status_down_at = status_up_at = None
    usb_required = not args.network_only
    last_usb = usb_path(args.usb_serial) is not None if usb_required else True
    last_available = True
    recovered_status = None
    sentinel_consecutive_failures = 0
    saw_sentinel_down = False
    last_sentinel_available = True

    while time.monotonic() < deadline:
        capture.poll()
        elapsed = time.monotonic() - started
        present = usb_path(args.usb_serial) is not None if usb_required else True
        if usb_required and present != last_usb:
            evidence.emit("trial_usb_transition", trial=trial, present=present, elapsed=elapsed)
            if not present:
                saw_usb_down = True
                usb_down_at = elapsed
            elif saw_usb_down:
                usb_up_at = elapsed
            last_usb = present

        status = parse_status(udp_command(address, "status", args.command_timeout))
        available = status is not None
        if available != last_available:
            evidence.emit(
                "trial_status_transition",
                trial=trial,
                available=available,
                elapsed=elapsed,
                status=status,
            )
            if not available:
                saw_status_down = True
                status_down_at = elapsed
            elif saw_status_down:
                status_up_at = elapsed
            last_available = available
        if args.sentinel_address:
            sentinel = parse_status(
                udp_command(args.sentinel_address, "status", args.command_timeout)
            )
            sentinel_available = bool(
                sentinel
                and sentinel.get("name") == args.sentinel_name
                and sentinel.get("infra") == 1
            )
            if sentinel_available:
                sentinel_consecutive_failures = 0
            else:
                sentinel_consecutive_failures += 1
            sentinel_healthy = sentinel_consecutive_failures < 3
            if sentinel_healthy != last_sentinel_available:
                evidence.emit(
                    "trial_sentinel_transition",
                    trial=trial,
                    available=sentinel_healthy,
                    elapsed=elapsed,
                    consecutive_failures=sentinel_consecutive_failures,
                    status=sentinel,
                )
                if not sentinel_healthy:
                    saw_sentinel_down = True
                last_sentinel_available = sentinel_healthy
        restarted = status_is_restarted(
            status, baseline_up, saw_usb_down, saw_status_down, usb_required
        )
        healthy = bool(status and status.get("name") == args.target and status.get("infra") == 1)
        if restarted and healthy and present and (saw_usb_down or saw_status_down):
            recovered_status = status
            break
        time.sleep(args.poll_interval)

    recovered_boot = (
        parse_boot_status(udp_command(address, "boot", args.command_timeout))
        if recovered_status
        else None
    )
    result = {
        "trial": trial,
        "success": recovered_status is not None,
        "duration": time.monotonic() - started,
        "baseline": baseline,
        "baseline_boot": baseline_boot,
        "recovered_status": recovered_status,
        "recovered_boot": recovered_boot,
        "saw_usb_down": saw_usb_down,
        "saw_status_down": saw_status_down,
        "usb_monitored": usb_required,
        "usb_down_at": usb_down_at,
        "usb_up_at": usb_up_at,
        "status_down_at": status_down_at,
        "status_up_at": status_up_at,
        "saw_sentinel_down": saw_sentinel_down,
    }
    result["failure_domain"] = failure_domain(recovered_status is not None, saw_sentinel_down)
    evidence.emit("trial_result", **result)
    (evidence.outdir / f"kernel-trial-{trial:03d}.log").write_text(kernel_log_since(wall_start))
    return result


def default_outdir(target: str) -> pathlib.Path:
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    return pathlib.Path.home() / "boot-tests" / f"{stamp}-{target}"


def json_safe_args(args) -> dict[str, object]:
    """Return argparse values in the form accepted by the evidence writer."""
    return {
        name: str(value) if isinstance(value, pathlib.Path) else value
        for name, value in vars(args).items()
    }


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", default=DEFAULT_CANARY)
    parser.add_argument("--canary-name", default=DEFAULT_CANARY)
    parser.add_argument("--usb-serial", default=DEFAULT_USB_SERIAL)
    parser.add_argument("--expected-address", default=DEFAULT_CANARY_ADDRESS)
    parser.add_argument("--sentinel-name")
    parser.add_argument("--sentinel-address")
    parser.add_argument(
        "--network-only",
        action="store_true",
        help="do not require or monitor local USB; exact name and IPv6 still gate reboot",
    )
    parser.add_argument(
        "--require-healthy",
        action="store_true",
        help="fail observation unless identity, image, infrastructure, and boot trail are healthy",
    )
    parser.add_argument("--expected-img", default="0.1.10")
    parser.add_argument("--dashboard", default=DASHBOARD)
    parser.add_argument("--action", choices=("observe", "reboot"), default="observe")
    parser.add_argument("--trials", type=int, default=1)
    parser.add_argument("--observe-seconds", type=float, default=5.0)
    parser.add_argument("--max-recovery-seconds", type=float, default=150.0)
    parser.add_argument("--settle-seconds", type=float, default=15.0)
    parser.add_argument("--poll-interval", type=float, default=0.25)
    parser.add_argument("--command-timeout", type=float, default=0.2)
    parser.add_argument("--control-key-dir", type=pathlib.Path)
    parser.add_argument("--allow-legacy-control", action="store_true")
    parser.add_argument("--out", type=pathlib.Path)
    args = parser.parse_args()
    if args.trials < 1 or args.trials > 100:
        parser.error("--trials must be between 1 and 100")
    if bool(args.sentinel_name) != bool(args.sentinel_address):
        parser.error("--sentinel-name and --sentinel-address must be used together")
    return args


def main() -> int:
    args = parse_args()
    outdir = args.out or default_outdir(args.target)
    evidence = Evidence(outdir)
    capture = SerialCapture(None if args.network_only else args.usb_serial, evidence)
    results = []
    exit_code = 0
    try:
        evidence.write_json("invocation.json", json_safe_args(args) | {"out": str(outdir)})
        address, _, _ = preflight(args, evidence)
        if args.action == "observe":
            observe(args, evidence, capture, address)
        else:
            for trial in range(1, args.trials + 1):
                result = run_trial(args, evidence, capture, address, trial)
                results.append(result)
                if not result["success"]:
                    evidence.emit(
                        "safety_stop",
                        reason=f"{result['failure_domain']} recovery failure",
                        trial=trial,
                    )
                    exit_code = 1
                    break
                if trial < args.trials:
                    time.sleep(args.settle_seconds)
                    # Re-run every identity and health preflight before another reset.
                    address, _, _ = preflight(args, evidence)
        final_state = dashboard_state(args.dashboard)
        evidence.write_json("dashboard-after.json", final_state)
    except Exception as exc:
        evidence.emit("fatal", error=str(exc), type=type(exc).__name__)
        print(f"boot_test: {exc}", file=sys.stderr)
        exit_code = 2
    finally:
        capture.close()
        evidence.write_json("result.json", {"exit_code": exit_code, "trials": results})
        evidence.close()
    print(f"evidence: {outdir}")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
