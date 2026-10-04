#!/usr/bin/env python3
"""Verify the exact SDK, Zephyr, module, and downstream patch inputs."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys


def output(*command: str) -> str:
    return subprocess.run(command, check=True, text=True, capture_output=True).stdout.strip()


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    root = pathlib.Path(__file__).resolve().parents[1]
    parser.add_argument("--workspace", type=pathlib.Path, default=root.parent / "zephyr-ws")
    parser.add_argument("--lock", type=pathlib.Path, default=root / "config/toolchain.lock.json")
    parser.add_argument("--profile", default=os.environ.get("TOOLCHAIN_PROFILE", "local"))
    parser.add_argument("--sdk", type=pathlib.Path)
    args = parser.parse_args()
    lock = json.loads(args.lock.read_text())
    errors: list[str] = []

    profiles = lock.get("tool_profiles", {})
    profile = profiles.get(args.profile)
    if not isinstance(profile, dict):
        print(f"workspace check: unknown tool profile {args.profile!r}", file=sys.stderr)
        return 1
    expected_image = profile.get("image")
    actual_image = os.environ.get("CI_JOB_IMAGE")
    if expected_image and actual_image and actual_image != expected_image:
        errors.append(f"CI image: expected {expected_image}, got {actual_image}")

    projects = {lock["zephyr"]["path"]: lock["zephyr"]["revision"], **lock["modules"]}
    patched_module_paths = {lock["mcuboot"]["path"]}
    for relative, expected in projects.items():
        path = args.workspace / relative
        try:
            actual = output("git", "-C", str(path), "rev-parse", "HEAD")
        except (OSError, subprocess.CalledProcessError):
            errors.append(f"missing Git project: {relative}")
            continue
        if actual != expected:
            errors.append(f"{relative}: expected {expected}, got {actual}")
        if relative != lock["zephyr"]["path"] and relative not in patched_module_paths:
            dirty = output("git", "-C", str(path), "status", "--porcelain=v1")
            if dirty:
                errors.append(f"{relative}: dependency checkout is dirty")

    patched = args.workspace / lock["zephyr"]["path"] / lock["zephyr"]["patched_file"]
    if not patched.is_file() or sha256(patched) != lock["zephyr"]["patched_file_sha256"]:
        errors.append("Zephyr USBD retry patch is absent or does not match the lock")
    zephyr_dirty = output(
        "git",
        "-C",
        str(args.workspace / lock["zephyr"]["path"]),
        "status",
        "--porcelain=v1",
    ).splitlines()
    expected_dirty = f"M {lock['zephyr']['patched_file']}"
    if zephyr_dirty != [expected_dirty]:
        errors.append(f"unexpected Zephyr worktree changes: {zephyr_dirty}")

    mcuboot = lock["mcuboot"]
    mcuboot_path = args.workspace / mcuboot["path"]
    for relative, expected_hash in mcuboot["patched_files"].items():
        patched_file = mcuboot_path / relative
        if not patched_file.is_file() or sha256(patched_file) != expected_hash:
            errors.append(f"MCUboot patch mismatch: {relative}")
    mcuboot_dirty = output("git", "-C", str(mcuboot_path), "status", "--porcelain=v1").splitlines()
    # The patch both modifies tracked files and adds new ones; git apply
    # leaves the additions untracked, which porcelain lists last as `??`.
    modified = []
    added = []
    for relative in sorted(mcuboot["patched_files"]):
        in_head = output(
            "git", "-C", str(mcuboot_path), "ls-tree", "--name-only", "HEAD", "--", relative
        )
        (modified if in_head else added).append(relative)
    expected_mcuboot_dirty = [
        f"{'M' if index == 0 else ' M'} {relative}" for index, relative in enumerate(modified)
    ] + [f"?? {relative}" for relative in added]
    if mcuboot_dirty != expected_mcuboot_dirty:
        errors.append(f"unexpected MCUboot worktree changes: {mcuboot_dirty}")

    tools = {
        "west": output(str(args.workspace / ".venv/bin/west"), "--version"),
        "cmake": output(str(args.workspace / ".venv/bin/cmake"), "--version").splitlines()[0],
        "python": output(str(args.workspace / ".venv/bin/python"), "--version"),
    }
    expected_fragments = {name: profile[name] for name in ("west", "cmake", "python")}
    for name, expected in expected_fragments.items():
        if expected not in tools[name]:
            errors.append(f"{name}: expected {expected}, got {tools[name]}")
    sdk = (args.sdk or args.workspace / f"zephyr-sdk-{lock['sdk']}").resolve()
    if not sdk.is_dir():
        errors.append(f"missing Zephyr SDK {lock['sdk']}: {sdk}")

    if errors:
        for error in errors:
            print(f"workspace check: {error}", file=sys.stderr)
        return 1
    print(
        json.dumps(
            {
                "ok": True,
                "profile": args.profile,
                "sdk": str(sdk),
                "workspace": str(args.workspace),
                "tools": tools,
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
