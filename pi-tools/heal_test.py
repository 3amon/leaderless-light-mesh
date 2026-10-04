#!/usr/bin/env python3
"""Prove the rejoin (self-heal) backoff on real hardware, over Thread.

The host tests in tests/c/heal_policy_test.c prove the SCHEDULE is right. They
cannot prove the device honours it: the counter has to survive the very reboot
it counts, and an earlier version kept it in RAM that MCUboot walks over, so it
silently reset every boot and the interval never escalated. Only hardware shows
that.

This drives the policy's INPUTS over the authenticated control channel rather
than physically isolating the ring, so Thread, telemetry and the admin channel
stay up the whole time and every step is observable:

    heal attempts <n>   jump straight to a step, so the 30 minute wait costs
                        seconds instead of half an hour
    heal alone 1        the neighbour table reads as empty
    heal fire           the interval has already elapsed

`alone` and `fire` are RAM-only on the device and die with the reboot they
cause, so a lost connection here cannot leave a ring reboot-looping.

Success means the boot ordinal advanced by exactly one per step, each with
reset reason `heal`, and the device reported the interval the schedule
requires.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import socket
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import control_client  # noqa: E402

STATUS_FIELD = re.compile(r"([a-z_]+)=([^\s]+)")
DEFAULT_INVENTORY = pathlib.Path("/opt/thread-led-node/config/rings.json")

# Step index -> interval the schedule must report (milliseconds).
EXPECTED_INTERVALS_MS = [60000, 180000, 600000, 1800000]


def status(address: str, timeout: float = 6.0) -> dict[str, str]:
    """Unauthenticated status query; returns {} when unreachable."""
    sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        sock.sendto(b"status", (address, 4242))
        raw = sock.recvfrom(512)[0].decode(errors="replace")
    except OSError:
        return {}
    finally:
        sock.close()
    return dict(STATUS_FIELD.findall(raw))


def wait_for_reboot(address: str, previous_boot: int, deadline_s: float) -> dict[str, str]:
    """Wait for the boot ordinal to advance. Returns the new status, or {}.

    A reboot makes the device unreachable for a while, so an empty reply is
    expected mid-flight and is not a failure until the deadline passes.
    """
    end = time.monotonic() + deadline_s
    while time.monotonic() < end:
        current = status(address, timeout=3.0)
        boot = current.get("boot", "")
        ordinal = int(boot.split("/")[0]) if boot.split("/")[0].isdigit() else -1
        if ordinal > previous_boot:
            return current
        time.sleep(2.0)
    return {}


def wait_until_settled(address: str, deadline_s: float = 60.0) -> dict[str, str]:
    """Wait for the ring to answer and report a steady state.

    Each step follows a reboot, and a ring that is still re-attaching drops
    datagrams. Issuing the next step into that window is how the first full run
    failed -- on a lost reply, not on a policy fault.
    """
    end = time.monotonic() + deadline_s
    latest: dict[str, str] = {}
    while time.monotonic() < end:
        latest = status(address, timeout=3.0)
        if latest.get("state") == "healthy":
            return latest
        time.sleep(2.0)
    return latest


def command(address: str, device: str, text: str, timeout: float,
            attempts: int = 4, delay: float = 3.0) -> str:
    """Authenticated command with retries.

    The control channel is UDP over a mesh that has just rebooted a node, so a
    dropped request or reply is expected rather than exceptional. Retrying is
    safe: every command here is idempotent, and the sequence number is re-read
    from the device on each attempt.
    """
    last: Exception | None = None
    for attempt in range(attempts):
        try:
            return control_client.authenticated_udp_command(address, device, text, timeout)
        except Exception as exc:  # noqa: BLE001 - any transport error is retryable
            last = exc
            if attempt < attempts - 1:
                time.sleep(delay)
    raise last  # type: ignore[misc]


def run(args: argparse.Namespace) -> dict:
    inventory = json.loads(args.inventory.read_text())
    devices = {d["name"]: d for d in inventory["devices"]}
    if args.target not in devices:
        raise SystemExit(f"unknown ring {args.target!r}")
    address = args.expected_address or devices[args.target]["address"]

    events: list[dict] = []

    def record(**fields):
        fields["t"] = round(time.monotonic() - started, 2)
        events.append(fields)
        print(json.dumps(fields), flush=True)

    started = time.monotonic()
    baseline = status(address)
    if not baseline:
        raise SystemExit(f"{args.target} is unreachable at {address}")
    record(event="baseline", boot=baseline.get("boot"), build=baseline.get("build"))

    failures: list[str] = []
    try:
        for step, expected_ms in enumerate(EXPECTED_INTERVALS_MS[: args.steps]):
            before = wait_until_settled(address)
            if not before:
                failures.append(f"step {step}: ring unreachable before staging")
                break
            before_boot = int(before.get("boot", "0/").split("/")[0] or 0)

            reply = command(address, args.target, f"heal attempts {step}", args.timeout)
            record(event="staged", step=step, reply=reply)

            reported = re.search(r"interval=(\d+)", reply)
            if not reported or int(reported.group(1)) != expected_ms:
                failures.append(
                    f"step {step}: device reported interval "
                    f"{reported.group(1) if reported else '?'} ms, expected {expected_ms}"
                )

            command(address, args.target, "heal alone 1", args.timeout)
            command(address, args.target, "heal fire", args.timeout)
            record(event="injected", step=step, expected_interval_ms=expected_ms)

            after = wait_for_reboot(address, before_boot, args.reboot_timeout)
            if not after:
                failures.append(f"step {step}: no reboot within {args.reboot_timeout}s")
                break

            after_boot = int(after.get("boot", "0/").split("/")[0] or 0)
            reason = after.get("reset", "")
            record(
                event="rebooted",
                step=step,
                boot=after_boot,
                reset=reason,
                up=after.get("up"),
            )
            if after_boot != before_boot + 1:
                failures.append(
                    f"step {step}: boot went {before_boot} -> {after_boot}, expected one"
                )
            if not reason.endswith("/heal"):
                failures.append(f"step {step}: reset reason {reason!r}, expected .../heal")

            # The injection is RAM-only, so the reboot already cleared it. Prove
            # that rather than assume it: a surviving override would be a loop.
            settled = status(address)
            if settled.get("state") not in {"healthy", "booting"}:
                failures.append(f"step {step}: unhealthy after reboot: {settled}")
    finally:
        # Always disarm, even on an exception, and restore a normal counter.
        try:
            command(address, args.target, "heal clear", args.timeout)
            command(address, args.target, "heal attempts 0", args.timeout)
            record(event="restored")
        except Exception as exc:  # noqa: BLE001 - cleanup must not mask failures
            failures.append(f"restore failed: {exc}")

    final = status(address)
    result = {
        "target": args.target,
        "address": address,
        "passed": not failures,
        "failures": failures,
        "final": final,
        "events": events,
    }
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2))
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True)
    parser.add_argument("--expected-address")
    parser.add_argument("--inventory", type=pathlib.Path, default=DEFAULT_INVENTORY)
    parser.add_argument("--steps", type=int, default=len(EXPECTED_INTERVALS_MS))
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument("--reboot-timeout", type=float, default=120.0)
    parser.add_argument("--out", type=pathlib.Path)
    args = parser.parse_args(argv)

    result = run(args)
    print(json.dumps({"passed": result["passed"], "failures": result["failures"]}, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
