#!/usr/bin/env python3
"""Reject generated build products that accidentally enter the Git index."""

from __future__ import annotations

import pathlib
import subprocess
import sys


def generated_reason(path: pathlib.PurePosixPath) -> str | None:
    parts = path.parts
    if "__pycache__" in parts or path.suffix == ".pyc":
        return "Python bytecode cache"
    if parts and parts[0] in {".cache", "ci-build", "dist"}:
        return "generated repository output"
    if len(parts) >= 3 and parts[:2] == ("firmware", "thread-ring"):
        if parts[2] == "build" or parts[2].startswith("build-"):
            return "thread-ring build output"
    if len(parts) >= 3 and parts[:3] == ("firmware", "ring-test", "build"):
        return "ring-test build output"
    return None


def main() -> int:
    tracked = subprocess.run(
        ["git", "ls-files", "-z"], check=True, capture_output=True
    ).stdout.split(b"\0")
    violations = []
    for raw_path in tracked:
        if not raw_path:
            continue
        path = pathlib.PurePosixPath(raw_path.decode("utf-8", errors="surrogateescape"))
        if reason := generated_reason(path):
            violations.append((path, reason))

    if violations:
        for path, reason in violations:
            print(f"repository hygiene: {path}: {reason}", file=sys.stderr)
        return 1
    print("repository hygiene: no generated files are tracked")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
