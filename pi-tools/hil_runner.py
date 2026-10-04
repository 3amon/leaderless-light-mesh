#!/usr/bin/env python3
"""Run indexed Pi hardware-in-loop jobs with explicit safety and retention."""

from __future__ import annotations

import argparse
import datetime as dt
import fcntl
import hashlib
import json
import os
import pathlib
import re
import shutil
import signal
import subprocess
import sys
import time
import uuid

SCHEMA = 1
SAFE_RISKS = {"observe", "diagnostic"}
ALL_RISKS = SAFE_RISKS | {"disruptive"}
RING_PLACEHOLDER = re.compile(r"\{ring:([^}:]+):(address|usb_serial)\}")
INPUT_PLACEHOLDER = re.compile(r"\{input:([^}:]+)\}")
INPUT_NAME = re.compile(r"[A-Za-z][A-Za-z0-9_-]{0,63}")


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


def load_config(path: pathlib.Path) -> dict:
    config = json.loads(path.read_text())
    if config.get("schema") != SCHEMA:
        raise ValueError(f"unsupported HIL config schema: {config.get('schema')!r}")
    if not isinstance(config.get("jobs"), dict) or not config["jobs"]:
        raise ValueError("HIL config must contain jobs")
    if not isinstance(config.get("inventory"), str) or not config["inventory"]:
        raise ValueError("HIL config must name an inventory")
    if not isinstance(config.get("root"), str) or not config["root"]:
        raise ValueError("HIL config must name an output root")
    retention = config.get("retention", {})
    for key in ("successful_runs_per_job", "failed_runs_per_job"):
        value = retention.get(key)
        if not isinstance(value, int) or value < 1:
            raise ValueError(f"retention {key} must be a positive integer")
    for name, job in config["jobs"].items():
        if not name or not isinstance(job, dict):
            raise ValueError("job names and definitions must be non-empty")
        if job.get("risk") not in ALL_RISKS:
            raise ValueError(f"job {name}: invalid risk")
        if not isinstance(job.get("argv"), list) or not job["argv"]:
            raise ValueError(f"job {name}: argv must be a non-empty list")
        if not all(isinstance(item, str) and item for item in job["argv"]):
            raise ValueError(f"job {name}: argv entries must be non-empty strings")
        timeout = job.get("timeout_seconds")
        if not isinstance(timeout, int | float) or timeout <= 0:
            raise ValueError(f"job {name}: timeout_seconds must be positive")
        schedules = job.get("schedules", [])
        if not isinstance(schedules, list) or not all(
            isinstance(item, str) and item for item in schedules
        ):
            raise ValueError(f"job {name}: schedules must be a string list")
        if job["risk"] == "disruptive" and schedules:
            raise ValueError(f"job {name}: disruptive jobs cannot be scheduled")
        inputs = job.get("inputs", [])
        if not isinstance(inputs, list) or not all(
            isinstance(item, str) and INPUT_NAME.fullmatch(item) for item in inputs
        ):
            raise ValueError(f"job {name}: inputs must be valid input names")
        if len(set(inputs)) != len(inputs):
            raise ValueError(f"job {name}: inputs must be unique")
        referenced_inputs = {
            match.group(1) for item in job["argv"] for match in INPUT_PLACEHOLDER.finditer(item)
        }
        if referenced_inputs != set(inputs):
            raise ValueError(
                f"job {name}: declared inputs do not match argv references: "
                f"{sorted(referenced_inputs)}"
            )
    return config


def config_sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def file_sha256(path: pathlib.Path) -> str | None:
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError:
        return None


def load_inventory(path: pathlib.Path) -> tuple[dict[str, dict], str]:
    inventory = json.loads(path.read_text())
    if inventory.get("schema") != 1 or not isinstance(inventory.get("devices"), list):
        raise ValueError("unsupported ring inventory")
    devices = {}
    for device in inventory["devices"]:
        name = device.get("name") if isinstance(device, dict) else None
        if not name or name in devices:
            raise ValueError("ring inventory names must be unique and non-empty")
        if not device.get("address") or not device.get("usb_serial"):
            raise ValueError(f"ring inventory entry is incomplete: {name}")
        devices[name] = device
    return devices, hashlib.sha256(path.read_bytes()).hexdigest()


def expand_argv(
    argv: list[str],
    repo: pathlib.Path,
    run_dir: pathlib.Path,
    devices: dict[str, dict],
    inputs: dict[str, pathlib.Path] | None = None,
) -> list[str]:
    values = {
        "python": sys.executable,
        "repo": str(repo),
        "run_dir": str(run_dir),
    }

    def ring_value(match: re.Match) -> str:
        name, field = match.groups()
        try:
            return str(devices[name][field])
        except KeyError as exc:
            raise ValueError(f"unknown ring inventory value: {name}.{field}") from exc

    def input_value(match: re.Match) -> str:
        name = match.group(1)
        try:
            return str((inputs or {})[name])
        except KeyError as exc:
            raise ValueError(f"required HIL input was not supplied: {name}") from exc

    try:
        return [
            INPUT_PLACEHOLDER.sub(input_value, RING_PLACEHOLDER.sub(ring_value, item)).format_map(
                values
            )
            for item in argv
        ]
    except KeyError as exc:
        raise ValueError(f"unknown argv placeholder: {exc.args[0]}") from exc


def atomic_json(path: pathlib.Path, value: dict) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def git_revision(repo: pathlib.Path) -> str | None:
    completed = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=repo,
        text=True,
        capture_output=True,
        check=False,
    )
    return completed.stdout.strip() if completed.returncode == 0 else None


def append_index(root: pathlib.Path, record: dict) -> None:
    with (root / "index.jsonl").open("a", buffering=1) as index:
        index.write(json.dumps(record, sort_keys=True) + "\n")


def terminate_process(process: subprocess.Popen) -> None:
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=5)
    except ProcessLookupError:
        process.wait(timeout=5)
        return
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=5)


def run_job(
    name: str,
    job: dict,
    *,
    root: pathlib.Path,
    repo: pathlib.Path,
    config_hash: str,
    devices: dict[str, dict],
    inventory_hash: str,
    allow_disruptive: bool,
    inputs: dict[str, pathlib.Path] | None = None,
) -> dict:
    if job["risk"] == "disruptive" and not allow_disruptive:
        raise PermissionError(f"job {name}: --allow-disruptive is required")

    started_wall = utc_now()
    started_monotonic = time.monotonic()
    run_id = (
        dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        + f"-{name}-{uuid.uuid4().hex[:8]}"
    )
    run_dir = root / "runs" / name / run_id
    required_inputs = set(job.get("inputs", []))
    supplied_inputs = inputs or {}
    missing_inputs = required_inputs - set(supplied_inputs)
    if missing_inputs:
        raise ValueError(f"job {name}: missing inputs: {sorted(missing_inputs)}")
    argv = expand_argv(job["argv"], repo, run_dir, devices, supplied_inputs)
    run_dir.mkdir(parents=True, exist_ok=False)
    metadata = {
        "schema": SCHEMA,
        "run_id": run_id,
        "job": name,
        "risk": job["risk"],
        "status": "running",
        "started": started_wall,
        "timeout_seconds": job["timeout_seconds"],
        "argv": argv,
        "repo_revision": git_revision(repo),
        "runner_sha256": file_sha256(pathlib.Path(__file__)),
        "job_script_sha256": file_sha256(pathlib.Path(argv[1])) if len(argv) > 1 else None,
        "config_sha256": config_hash,
        "inventory_sha256": inventory_hash,
        "inputs": {
            input_name: {
                "path": str(supplied_inputs[input_name]),
                "sha256": file_sha256(supplied_inputs[input_name]),
            }
            for input_name in sorted(required_inputs)
        },
        "run_dir": str(run_dir),
    }
    atomic_json(run_dir / "run.json", metadata)

    timed_out = False
    with (
        (run_dir / "stdout.log").open("wb") as stdout,
        (run_dir / "stderr.log").open("wb") as stderr,
    ):
        try:
            process = subprocess.Popen(
                argv,
                cwd=repo,
                stdout=stdout,
                stderr=stderr,
                start_new_session=True,
            )
            try:
                return_code = process.wait(timeout=job["timeout_seconds"])
            except subprocess.TimeoutExpired:
                timed_out = True
                terminate_process(process)
                return_code = process.returncode
        except OSError as exc:
            return_code = None
            metadata["error"] = f"{type(exc).__name__}: {exc}"

    metadata.update(
        {
            "status": "timeout" if timed_out else "passed" if return_code == 0 else "failed",
            "return_code": return_code,
            "timed_out": timed_out,
            "finished": utc_now(),
            "duration_seconds": round(time.monotonic() - started_monotonic, 3),
        }
    )
    atomic_json(run_dir / "run.json", metadata)
    append_index(root, metadata)
    return metadata


def retained_runs(job_dir: pathlib.Path, status: str) -> list[pathlib.Path]:
    matches = []
    if not job_dir.is_dir():
        return matches
    for run_dir in job_dir.iterdir():
        metadata_path = run_dir / "run.json"
        if run_dir.is_symlink() or not run_dir.is_dir() or not metadata_path.is_file():
            continue
        try:
            metadata = json.loads(metadata_path.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        successful = metadata.get("status") == "passed"
        if (status == "passed") == successful:
            matches.append((str(metadata.get("started", "")), run_dir))
    return [path for _, path in sorted(matches, reverse=True)]


def apply_retention(root: pathlib.Path, config: dict) -> list[pathlib.Path]:
    removed = []
    retention = config["retention"]
    runs_root = root / "runs"
    for name in config["jobs"]:
        job_dir = runs_root / name
        groups = (
            ("passed", retention["successful_runs_per_job"]),
            ("failed", retention["failed_runs_per_job"]),
        )
        for status, keep in groups:
            for run_dir in retained_runs(job_dir, status)[keep:]:
                try:
                    run_dir.relative_to(runs_root)
                except ValueError as exc:
                    raise RuntimeError(f"unsafe retention path: {run_dir}") from exc
                shutil.rmtree(run_dir)
                removed.append(run_dir)
                append_index(
                    root,
                    {
                        "schema": SCHEMA,
                        "event": "retention_delete",
                        "job": name,
                        "run_dir": str(run_dir),
                        "deleted": utc_now(),
                    },
                )
    return removed


def select_jobs(config: dict, job_name: str | None, schedule: str | None) -> list[str]:
    if job_name:
        if job_name not in config["jobs"]:
            raise ValueError(f"unknown job: {job_name}")
        return [job_name]
    selected = [
        name
        for name, job in config["jobs"].items()
        if job.get("enabled", False) and schedule in job.get("schedules", [])
    ]
    if not selected:
        raise ValueError(f"schedule has no enabled jobs: {schedule}")
    return selected


def resolve_root(value: pathlib.Path, repo: pathlib.Path) -> pathlib.Path:
    root = value.expanduser().resolve()
    forbidden = {pathlib.Path("/"), pathlib.Path.home().resolve(), repo.resolve()}
    if root in forbidden:
        raise ValueError(f"refusing unsafe HIL root: {root}")
    return root


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=pathlib.Path, required=True)
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument("--job")
    selection.add_argument("--schedule")
    parser.add_argument("--allow-disruptive", action="store_true")
    parser.add_argument(
        "--input",
        action="append",
        default=[],
        metavar="NAME=PATH",
        help="bind a named, hashed input file used by the selected job",
    )
    parser.add_argument("--root", type=pathlib.Path)
    return parser.parse_args()


def parse_inputs(values: list[str]) -> dict[str, pathlib.Path]:
    inputs: dict[str, pathlib.Path] = {}
    for value in values:
        name, separator, raw_path = value.partition("=")
        if not separator or not INPUT_NAME.fullmatch(name) or not raw_path:
            raise ValueError(f"invalid HIL input binding: {value!r}")
        if name in inputs:
            raise ValueError(f"duplicate HIL input binding: {name}")
        path = pathlib.Path(raw_path).expanduser().resolve()
        if not path.is_file():
            raise ValueError(f"HIL input is not a file: {path}")
        inputs[name] = path
    return inputs


def main() -> int:
    args = parse_args()
    repo = pathlib.Path(__file__).resolve().parents[1]
    try:
        config = load_config(args.config)
        inventory_path = (repo / config["inventory"]).resolve()
        inventory_path.relative_to(repo)
        devices, inventory_hash = load_inventory(inventory_path)
        root = resolve_root(args.root or pathlib.Path(config["root"]), repo)
        inputs = parse_inputs(args.input)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"HIL runner: {exc}", file=sys.stderr)
        return 2
    try:
        root.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        print(f"HIL runner: cannot create output root: {exc}", file=sys.stderr)
        return 2
    lock_path = root / "runner.lock"
    with lock_path.open("a+") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print("HIL runner is already active", file=sys.stderr)
            return 75
        try:
            names = select_jobs(config, args.job, args.schedule)
            results = [
                run_job(
                    name,
                    config["jobs"][name],
                    root=root,
                    repo=repo,
                    config_hash=config_sha256(args.config),
                    devices=devices,
                    inventory_hash=inventory_hash,
                    allow_disruptive=args.allow_disruptive,
                    inputs=inputs,
                )
                for name in names
            ]
            apply_retention(root, config)
        except (OSError, RuntimeError, ValueError, PermissionError) as exc:
            print(f"HIL runner: {exc}", file=sys.stderr)
            return 2
    print(json.dumps(results, indent=2, sort_keys=True))
    return 0 if all(result["status"] == "passed" for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
