#!/usr/bin/env python3
"""Measure a crowd -> singleton/resident split -> crowd rejoin.

By default the tool uses authenticated ``gossip realm``/``gossip both``
commands on one wanderer. Realm-only transmit suppresses its direct-proximity
advertisement without taking Thread, clock gossip receive, OTA, or management
offline. The original physical-movement workflow remains available with
``--manual``.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import control_client
import thread_socket
from rotation_test import status

import ringctl

PORT = 4242
GROUP_HEADER = re.compile(
    r"^sid=(?P<sid>\d+) "
    r"view=(?P<view>[0-9a-fA-F]{8}) "
    r"target=(?P<target>[0-9a-fA-F]{8}) "
    r"topo=(?P<topology_complete>[01]) "
    r"agreed=(?P<agreed>[0-9a-fA-F]{8}) "
    r"commit=(?P<committed>[0-9a-fA-F]{8}) "
    r"hand=(?P<handoff_ticks>\d+) "
    r"lit=(?P<lit>[01]) "
    r"vis=(?P<visibility>\d+\.\d+) "
    r"fade=(?P<fade>\d+\.\d+) "
    r"size=(?P<size>\d+) "
    r"tier=(?P<tier>[a-z]+)"
)


def udp_query(address: str, command: str, timeout: float = 1.0) -> str | None:
    sock = thread_socket.udp_socket(address, timeout)
    try:
        sock.sendto(command.encode("ascii"), (address, PORT))
        return sock.recv(768).decode(errors="replace")
    except (OSError, socket.timeout):
        return None
    finally:
        sock.close()


def parse_group(reply: str | None) -> dict:
    if not reply:
        return {"reply": None}
    match = GROUP_HEADER.match(reply)
    if not match:
        return {"reply": reply, "parse_error": True}
    parsed: dict[str, object] = {"reply": reply}
    for key, value in match.groupdict().items():
        if key in {"tier"}:
            parsed[key] = value
        elif key in {"visibility", "fade"}:
            parsed[key] = float(value)
        elif key in {"view", "target", "agreed", "committed"}:
            parsed[key] = int(value, 16)
        else:
            parsed[key] = int(value)
    return parsed


def tier_for_size(size: int) -> str:
    if size <= 1:
        return "solo"
    return "duo" if size == 2 else "crowd"


def phase_matches(
    samples: dict[str, dict],
    names: list[str],
    wanderer: str,
    phase: str,
) -> bool:
    if any("sid" not in samples.get(name, {}) for name in names):
        return False
    sid_bits = {name: 1 << int(samples[name]["sid"]) for name in names}
    all_bits = 0
    for bit in sid_bits.values():
        all_bits |= bit

    if phase == "crowd":
        return all(
            sample["topology_complete"] == 1
            and sample["target"] == all_bits
            and sample["agreed"] == all_bits
            and sample["committed"] == all_bits
            and sample["handoff_ticks"] == 0
            and sample["lit"] == 1
            and sample["visibility"] >= 0.99
            and sample["fade"] >= 0.99
            and sample["size"] == len(names)
            and sample["tier"] == tier_for_size(len(names))
            for sample in samples.values()
        )
    if phase != "split":
        raise ValueError(f"unknown phase: {phase}")

    resident_bits = all_bits & ~sid_bits[wanderer]
    for name, sample in samples.items():
        expected = sid_bits[name] if name == wanderer else resident_bits
        expected_lit = 0 if name == wanderer else 1
        expected_size = 1 if name == wanderer else len(names) - 1
        expected_tier = tier_for_size(expected_size)
        if not (
            sample["topology_complete"] == 1
            and sample["target"] == expected
            and sample["agreed"] == expected
            and sample["committed"] == (0 if name == wanderer else expected)
            and sample["handoff_ticks"] == 0
            and sample["lit"] == expected_lit
            and (sample["visibility"] <= 0.01 if name == wanderer else sample["visibility"] >= 0.99)
            and (sample["fade"] >= 0.99 if name != wanderer else True)
            and sample["size"] == expected_size
            and sample["tier"] == expected_tier
        ):
            return False
    return True


def poll_groups(names: list[str], addresses: dict[str, str], timeout: float) -> dict[str, dict]:
    def query(name: str) -> tuple[str, dict]:
        return name, parse_group(udp_query(addresses[name], "group", timeout))

    with concurrent.futures.ThreadPoolExecutor(max_workers=min(8, len(names))) as executor:
        return dict(executor.map(query, names))


def resident_continuity(
    records: list[dict], names: list[str], wanderer: str, max_missing: int
) -> dict[str, object]:
    residents = [name for name in names if name != wanderer]
    resident_bits = 0
    for name in residents:
        for record in records:
            sample = record.get("samples", {}).get(name, {})
            if "sid" in sample:
                resident_bits |= 1 << int(sample["sid"])
                break

    missing = 0
    violations: list[dict[str, object]] = []
    max_handoff_ticks = {name: 0 for name in residents}
    committed_sets = {name: set() for name in residents}
    for record in records:
        if record.get("phase") not in {"split", "rejoined"}:
            continue
        for name in residents:
            sample = record.get("samples", {}).get(name, {})
            if "sid" not in sample:
                missing += 1
                continue
            max_handoff_ticks[name] = max(
                max_handoff_ticks[name], int(sample.get("handoff_ticks", 0))
            )
            committed = int(sample.get("committed", 0))
            committed_sets[name].add(f"{committed:08x}")
            reasons = []
            if sample.get("lit") != 1:
                reasons.append("group lock went dark")
            if float(sample.get("visibility", 0)) < 0.99:
                reasons.append("group visibility fell below 0.99")
            if sample.get("tier") == "solo" or int(sample.get("size", 1)) < 2:
                reasons.append("resident exposed solo state")
            if committed & resident_bits != resident_bits:
                reasons.append("committed scene omitted a resident")
            if reasons:
                violations.append({"t": record["t"], "ring": name, "reasons": reasons})

    reasons = []
    if missing > max_missing:
        reasons.append(f"missing resident samples {missing} > {max_missing}")
    if violations:
        reasons.append(f"resident continuity violations: {len(violations)}")
    if not any(max_handoff_ticks.values()):
        reasons.append("no resident reported a committed-scene handoff")
    return {
        "pass": not reasons,
        "reasons": reasons,
        "missing_samples": missing,
        "max_handoff_ticks": max_handoff_ticks,
        "committed_sets": {name: sorted(values) for name, values in committed_sets.items()},
        "violations": violations,
    }


def wait_phase(
    phase: str,
    names: list[str],
    addresses: dict[str, str],
    wanderer: str,
    timeout: float,
    poll: float,
    telemetry,
    started: float,
    records: list[dict],
) -> dict[str, dict]:
    deadline = time.monotonic() + timeout
    next_poll = time.monotonic()
    while time.monotonic() < deadline:
        samples = poll_groups(names, addresses, min(1.0, max(poll, 0.25)))
        record = {
            "t": time.monotonic() - started,
            "phase": phase,
            "samples": samples,
        }
        records.append(record)
        telemetry.write(json.dumps(record, sort_keys=True) + "\n")
        if phase_matches(samples, names, wanderer, phase):
            return samples
        next_poll = max(next_poll + poll, time.monotonic())
        time.sleep(max(0, next_poll - time.monotonic()))
    raise RuntimeError(f"{phase} did not converge within {timeout:.1f}s")


def set_gossip(
    name: str,
    address: str,
    mode: str,
    timeout: float,
    key_dir: pathlib.Path | None,
) -> str:
    return control_client.authenticated_udp_command(
        address,
        name,
        f"gossip {mode}",
        timeout,
        key_path=control_client.key_path_for(name, key_dir),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--rings",
        default="opal-reef-tide,feral-tide-light,lucid-iris-light",
    )
    parser.add_argument("--wanderer", default="opal-reef-tide")
    parser.add_argument("--phase-timeout", type=float, default=60)
    parser.add_argument("--poll", type=float, default=0.5)
    parser.add_argument("--manual", action="store_true")
    parser.add_argument("--control-key-dir", type=pathlib.Path)
    parser.add_argument("--mutation-timeout", type=float, default=5)
    parser.add_argument("--max-missing-resident-samples", type=int, default=2)
    parser.add_argument("--outdir", required=True)
    args = parser.parse_args()
    names = [name.strip() for name in args.rings.split(",") if name.strip()]
    if len(names) < 3 or len(names) > 9 or len(set(names)) != len(names):
        parser.error("--rings must contain 3-9 unique names")
    if args.wanderer not in names:
        parser.error("--wanderer must be one of --rings")
    if args.phase_timeout <= 0 or args.poll < 0.1 or args.mutation_timeout <= 0:
        parser.error("--phase-timeout must be positive and --poll at least 0.1")
    if args.max_missing_resident_samples < 0:
        parser.error("--max-missing-resident-samples cannot be negative")

    addresses = {name: ringctl.resolve(name) for name in names}
    os.makedirs(args.outdir, exist_ok=False)
    telemetry_path = os.path.join(args.outdir, "telemetry.jsonl")
    result: dict[str, object] = {
        "pass": False,
        "rings": names,
        "wanderer": args.wanderer,
        "addresses": addresses,
        "disturbance": "manual" if args.manual else "authenticated-gossip",
        "phases": {},
    }
    initial_status = {name: status(address, 1.0) for name, address in addresses.items()}
    result["initial_status"] = initial_status
    started = time.monotonic()
    records: list[dict] = []
    disturbance_attempted = False

    try:
        with open(telemetry_path, "w", buffering=1) as telemetry:
            result["phases"]["baseline"] = wait_phase(
                "crowd",
                names,
                addresses,
                args.wanderer,
                args.phase_timeout,
                args.poll,
                telemetry,
                started,
                records,
            )
            if args.manual:
                print(
                    f"baseline crowd ready; move {args.wanderer} out of direct radio visibility",
                    flush=True,
                )
            else:
                disturbance_attempted = True
                result["split_command"] = set_gossip(
                    args.wanderer,
                    addresses[args.wanderer],
                    "realm",
                    args.mutation_timeout,
                    args.control_key_dir,
                )
            result["phases"]["split"] = wait_phase(
                "split",
                names,
                addresses,
                args.wanderer,
                args.phase_timeout,
                args.poll,
                telemetry,
                started,
                records,
            )
            if args.manual:
                print(
                    f"split ready; return {args.wanderer} to direct radio visibility",
                    flush=True,
                )
            else:
                result["rejoin_command"] = set_gossip(
                    args.wanderer,
                    addresses[args.wanderer],
                    "both",
                    args.mutation_timeout,
                    args.control_key_dir,
                )
            result["phases"]["rejoined"] = wait_phase(
                "crowd",
                names,
                addresses,
                args.wanderer,
                args.phase_timeout,
                args.poll,
                telemetry,
                started,
                records,
            )
        final_status = {name: status(address, 1.0) for name, address in addresses.items()}
        result["final_status"] = final_status
        changed_boots = [
            name
            for name in names
            if initial_status[name].get("boot_sequence") != final_status[name].get("boot_sequence")
        ]
        if changed_boots:
            raise RuntimeError(f"boot sequence changed: {changed_boots}")
        result["resident_continuity"] = resident_continuity(
            records,
            names,
            args.wanderer,
            args.max_missing_resident_samples,
        )
        if not result["resident_continuity"]["pass"]:
            raise RuntimeError("; ".join(result["resident_continuity"]["reasons"]))
        result["pass"] = True
    except Exception as exc:
        result["error"] = str(exc)
    finally:
        if disturbance_attempted and not args.manual:
            try:
                result["cleanup_command"] = set_gossip(
                    args.wanderer,
                    addresses[args.wanderer],
                    "both",
                    args.mutation_timeout,
                    args.control_key_dir,
                )
            except Exception as cleanup_exc:
                result["cleanup_error"] = str(cleanup_exc)
                result["pass"] = False
        result["duration_seconds"] = time.monotonic() - started
        with open(os.path.join(args.outdir, "result.json"), "w") as output:
            json.dump(result, output, indent=2, sort_keys=True)

    print(json.dumps(result, indent=2, sort_keys=True))
    print(f"run dir: {args.outdir}")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
