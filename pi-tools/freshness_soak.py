#!/usr/bin/env python3
"""Run a camera-independent peer-freshness soak with explicit pass/fail gates."""

import argparse
import json
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import thread_socket

import ringctl
from status_telemetry import evaluate_freshness, parse_status

PORT = 4242
DEFAULT_RINGS = (
    "opal-reef-tide",
    "feral-tide-light",
    "lucid-iris-light",
    "glowing-bloom-wave",
    "dusty-fern-shimmer",
)


def status(address, timeout):
    sock = thread_socket.udp_socket(address, timeout)
    try:
        sock.sendto(b"status", (address, PORT))
        return sock.recv(256).decode()
    except (OSError, socket.timeout):
        return None
    finally:
        sock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rings", default=",".join(DEFAULT_RINGS))
    parser.add_argument("--duration", type=float, default=300)
    parser.add_argument("--poll", type=float, default=0.5)
    parser.add_argument("--expected-fresh", type=int, default=None)
    parser.add_argument("--max-stale-samples", type=int, default=0)
    parser.add_argument("--max-missing-samples", type=int, default=0)
    parser.add_argument("--max-lapse-delta", type=int, default=0)
    parser.add_argument("--outdir", default=None)
    args = parser.parse_args()
    if args.duration <= 0 or args.poll <= 0:
        parser.error("--duration and --poll must be positive")

    names = [name.strip() for name in args.rings.split(",") if name.strip()]
    expected_fresh = len(names) - 1 if args.expected_fresh is None else args.expected_fresh
    addresses = {name: ringctl.resolve(name) for name in names}
    stamp = time.strftime("%Y%m%d-%H%M%S")
    outdir = args.outdir or os.path.expanduser(f"~/sync-tests/{stamp}-freshness")
    os.makedirs(outdir, exist_ok=True)
    records = []
    deadline = time.monotonic() + args.duration
    next_poll = time.monotonic()
    telemetry_path = os.path.join(outdir, "telemetry.jsonl")
    with open(telemetry_path, "w", buffering=1) as telemetry:
        while time.monotonic() < deadline:
            for name, address in addresses.items():
                reply = status(address, timeout=min(1.0, args.poll))
                record = {"t": time.clock_gettime(time.CLOCK_BOOTTIME), "ring": name}
                record.update(parse_status(reply))
                records.append(record)
                telemetry.write(json.dumps(record) + "\n")
            next_poll = max(next_poll + args.poll, time.monotonic())
            time.sleep(max(0, next_poll - time.monotonic()))

    result = evaluate_freshness(
        records,
        names,
        expected_fresh,
        args.max_stale_samples,
        args.max_missing_samples,
        args.max_lapse_delta,
    )
    result["duration_s"] = args.duration
    result["poll_s"] = args.poll
    result["addresses"] = addresses
    with open(os.path.join(outdir, "result.json"), "w") as output:
        json.dump(result, output, indent=2, sort_keys=True)
    print(json.dumps(result, indent=2, sort_keys=True))
    print(f"run dir: {outdir}")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
