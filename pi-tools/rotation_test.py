#!/usr/bin/env python3
"""Verify synchronized one-minute pattern rotation without using the camera."""

from __future__ import annotations

import argparse
import json
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import thread_socket

import ringctl
from status_telemetry import parse_status

PORT = 4242
DEFAULT_RINGS = (
    "opal-reef-tide",
    "feral-tide-light",
    "lucid-iris-light",
    "glowing-bloom-wave",
    "dusty-fern-shimmer",
)


def status(address: str, timeout: float) -> dict:
    sock = thread_socket.udp_socket(address, timeout)
    try:
        sock.sendto(b"status", (address, PORT))
        return parse_status(sock.recv(384).decode(errors="replace"))
    except (OSError, socket.timeout):
        return parse_status(None)
    finally:
        sock.close()


def evaluate_rotation(
    records: list[dict],
    names: list[str],
    *,
    min_changes: int,
    min_interval: float,
    max_interval: float,
    max_mismatch_cycles: int,
    max_missing_samples: int,
) -> dict:
    missing = 0
    identity_mismatches = 0
    mismatch_cycles = 0
    transition_times = []
    last_pattern = None
    boot_sequences = {name: [] for name in names}
    valid_cycles = 0

    for record in records:
        samples = record["samples"]
        patterns = []
        for name in names:
            sample = samples.get(name, {})
            if not sample.get("reply") or "pattern" not in sample:
                missing += 1
                continue
            if sample.get("name") != name:
                identity_mismatches += 1
                continue
            patterns.append(sample["pattern"])
            if "boot_sequence" in sample:
                boot_sequences[name].append(sample["boot_sequence"])
        if len(patterns) != len(names):
            continue
        valid_cycles += 1
        if len(set(patterns)) != 1:
            mismatch_cycles += 1
            continue
        pattern = patterns[0]
        if last_pattern is not None and pattern != last_pattern:
            transition_times.append(record["t"])
        last_pattern = pattern

    intervals = [later - earlier for earlier, later in zip(transition_times, transition_times[1:])]
    reasons = []
    if missing > max_missing_samples:
        reasons.append(f"missing samples {missing} > {max_missing_samples}")
    if identity_mismatches:
        reasons.append(f"identity mismatches {identity_mismatches}")
    if mismatch_cycles > max_mismatch_cycles:
        reasons.append(f"mismatch cycles {mismatch_cycles} > {max_mismatch_cycles}")
    if len(transition_times) < min_changes:
        reasons.append(f"pattern changes {len(transition_times)} < {min_changes}")
    bad_intervals = [value for value in intervals if value < min_interval or value > max_interval]
    if bad_intervals:
        reasons.append(f"rotation intervals outside [{min_interval}, {max_interval}]")
    for name, boots in boot_sequences.items():
        if not boots:
            reasons.append(f"{name} did not report a boot sequence")
        elif min(boots) != max(boots):
            reasons.append(f"{name} boot sequence changed {boots[0]} -> {boots[-1]}")

    boot_summary = {
        name: {
            "samples": len(boots),
            "start": boots[0] if boots else None,
            "end": boots[-1] if boots else None,
        }
        for name, boots in boot_sequences.items()
    }

    return {
        "pass": not reasons,
        "reasons": reasons,
        "samples": len(records),
        "valid_cycles": valid_cycles,
        "missing_samples": missing,
        "identity_mismatches": identity_mismatches,
        "mismatch_cycles": mismatch_cycles,
        "pattern_changes": len(transition_times),
        "transition_times": transition_times,
        "intervals_seconds": intervals,
        "boot_sequences": boot_summary,
        "thresholds": {
            "min_changes": min_changes,
            "min_interval": min_interval,
            "max_interval": max_interval,
            "max_mismatch_cycles": max_mismatch_cycles,
            "max_missing_samples": max_missing_samples,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--rings",
        default=",".join(DEFAULT_RINGS),
    )
    parser.add_argument("--duration", type=float, default=130)
    parser.add_argument("--poll", type=float, default=0.5)
    parser.add_argument("--min-changes", type=int, default=2)
    parser.add_argument("--min-interval", type=float, default=55)
    parser.add_argument("--max-interval", type=float, default=65)
    parser.add_argument("--max-mismatch-cycles", type=int, default=1)
    parser.add_argument("--max-missing-samples", type=int, default=3)
    parser.add_argument("--outdir", required=True)
    args = parser.parse_args()
    if args.duration <= 0 or args.duration > 3600:
        parser.error("--duration must be greater than zero and no more than 3600 seconds")
    if args.poll < 0.1:
        parser.error("--poll must be at least 0.1 seconds")
    if args.min_changes < 1 or args.min_interval <= 0 or args.max_interval < args.min_interval:
        parser.error("invalid rotation thresholds")
    if args.max_mismatch_cycles < 0 or args.max_missing_samples < 0:
        parser.error("missing and mismatch thresholds cannot be negative")

    names = [name.strip() for name in args.rings.split(",") if name.strip()]
    addresses = {name: ringctl.resolve(name) for name in names}
    os.makedirs(args.outdir, exist_ok=False)
    telemetry_path = os.path.join(args.outdir, "telemetry.jsonl")
    records = []
    started = time.monotonic()
    deadline = started + args.duration
    next_poll = started
    with open(telemetry_path, "w", buffering=1) as telemetry:
        while time.monotonic() < deadline:
            record = {"t": time.monotonic() - started, "samples": {}}
            for name, address in addresses.items():
                record["samples"][name] = status(address, min(1.0, args.poll))
            records.append(record)
            telemetry.write(json.dumps(record, sort_keys=True) + "\n")
            next_poll = max(next_poll + args.poll, time.monotonic())
            time.sleep(max(0, next_poll - time.monotonic()))

    result = evaluate_rotation(
        records,
        names,
        min_changes=args.min_changes,
        min_interval=args.min_interval,
        max_interval=args.max_interval,
        max_mismatch_cycles=args.max_mismatch_cycles,
        max_missing_samples=args.max_missing_samples,
    )
    result["duration_seconds"] = args.duration
    result["poll_seconds"] = args.poll
    result["addresses"] = addresses
    with open(os.path.join(args.outdir, "result.json"), "w") as output:
        json.dump(result, output, indent=2, sort_keys=True)
    print(json.dumps(result, indent=2, sort_keys=True))
    print(f"run dir: {args.outdir}")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
