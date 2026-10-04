#!/usr/bin/env python3
"""Derive MCUboot and diagnostic versions from Git state."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess

TAG = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")
VERSION = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:\+(\d+))?$")


def git(root: pathlib.Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(root), *args],
        check=True,
        text=True,
        capture_output=True,
    ).stdout.strip()


def derive(root: pathlib.Path, override: str | None = None) -> dict[str, object]:
    commit = git(root, "rev-parse", "HEAD")
    dirty = bool(git(root, "status", "--porcelain=v1", "--untracked-files=normal"))
    exact_tags = [
        tag for tag in git(root, "tag", "--points-at", "HEAD").splitlines() if TAG.fullmatch(tag)
    ]
    exact_tag = sorted(exact_tags)[-1] if exact_tags else None
    count = int(git(root, "rev-list", "--count", "HEAD"))

    if override:
        if not VERSION.fullmatch(override):
            raise ValueError("version override must be MAJOR.MINOR.PATCH[+BUILD]")
        image_version = override
    elif exact_tag:
        image_version = exact_tag[1:]
    else:
        try:
            base_tag = git(root, "describe", "--tags", "--match", "v[0-9]*", "--abbrev=0")
        except subprocess.CalledProcessError:
            base_tag = "v0.0.0"
        match = TAG.fullmatch(base_tag)
        base = ".".join(match.groups()) if match else "0.0.0"
        image_version = f"{base}+{count}"

    try:
        describe = git(root, "describe", "--tags", "--match", "v[0-9]*", "--always", "--dirty")
    except subprocess.CalledProcessError:
        describe = commit[:12] + ("-dirty" if dirty else "")
    return {
        "version": image_version,
        "build_id": describe,
        "commit": commit,
        "dirty": dirty,
        "exact_tag": exact_tag,
        "commit_count": count,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[1]
    )
    parser.add_argument("--override")
    parser.add_argument("--require-release", action="store_true")
    parser.add_argument("--format", choices=("json", "conf"), default="json")
    args = parser.parse_args()
    info = derive(args.repo, args.override)
    if args.require_release:
        if info["dirty"]:
            parser.error("release source tree is dirty")
        if not info["exact_tag"]:
            parser.error("HEAD has no exact vMAJOR.MINOR.PATCH release tag")
        if args.override:
            parser.error("release versions cannot be overridden")
    if args.format == "json":
        print(json.dumps(info, indent=2, sort_keys=True))
    else:
        print(f'CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="{info["version"]}"')
        print(f'CONFIG_THREAD_RING_BUILD_ID="{info["build_id"]}"')
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
