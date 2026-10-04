#!/usr/bin/env python3
"""Validate private signing keys and sign/verify release metadata."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import stat
import subprocess
import sys

MCUBOOT_DEVELOPMENT_PUBLIC_HASHES = {
    "fc5701dc6135e1323847bdc40f04d2e5bee5833b23c29f93593d00018cfa9994",
}
ATTESTATION_NAMESPACE = "thread-led-release"
# Identity listed in config/release-signers (see config/release-signers.example).
ATTESTATION_IDENTITY = "release@example.net"


def private_file(path: pathlib.Path, description: str) -> pathlib.Path:
    path = path.expanduser().resolve()
    if not path.is_file():
        raise ValueError(f"{description} does not exist: {path}")
    mode = stat.S_IMODE(path.stat().st_mode)
    if os.name == "posix" and mode & 0o077:
        raise ValueError(
            f"{description} must not be accessible by group/other: {path} has mode {mode:04o}"
        )
    return path


def firmware_key_info(workspace: pathlib.Path, key: pathlib.Path) -> dict[str, str]:
    key = private_file(key, "firmware signing key")
    workspace = workspace.expanduser().resolve()
    python = workspace / ".venv/bin/python"
    imgtool = workspace / "bootloader/mcuboot/scripts/imgtool.py"
    result = subprocess.run(
        [str(python), str(imgtool), "getpubhash", "-k", str(key), "-e", "raw"],
        check=True,
        capture_output=True,
    )
    public_hash = result.stdout.hex()
    if len(result.stdout) != 32:
        raise ValueError("imgtool returned an invalid firmware public-key hash")
    if public_hash in MCUBOOT_DEVELOPMENT_PUBLIC_HASHES:
        raise ValueError("MCUboot development signing key is forbidden")
    return {"path": str(key), "public_key_sha256": public_hash}


def sign_and_verify(
    path: pathlib.Path,
    key: pathlib.Path,
    allowed_signers: pathlib.Path,
) -> pathlib.Path:
    key = private_file(key, "release attestation key")
    signature = pathlib.Path(f"{path}.sig")
    subprocess.run(
        ["ssh-keygen", "-Y", "sign", "-q", "-f", str(key), "-n", ATTESTATION_NAMESPACE, str(path)],
        check=True,
    )
    subprocess.run(
        [
            "ssh-keygen",
            "-Y",
            "verify",
            "-f",
            str(allowed_signers),
            "-I",
            ATTESTATION_IDENTITY,
            "-n",
            ATTESTATION_NAMESPACE,
            "-s",
            str(signature),
        ],
        input=path.read_bytes(),
        check=True,
        capture_output=True,
    )
    return signature


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=pathlib.Path, required=True)
    parser.add_argument("--key", type=pathlib.Path, required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(firmware_key_info(args.workspace, args.key), sort_keys=True))
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"signing: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
