#!/usr/bin/env python3
"""Build, reproduce, verify, and package a strict tagged firmware release."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys

import signing
import version_info

SIGNATURE_TLVS = {0x20, 0x22, 0x23, 0x24, 0x25}
UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
RECOVERY_START = 0x26000
RECOVERY_END = 0x90000
UF2_BOOTLOADER_APP_END = 0xAD000


def run(command: list[str], *, cwd: pathlib.Path, env=None) -> str:
    return subprocess.run(
        command, cwd=cwd, env=env, check=True, text=True, capture_output=True
    ).stdout


def digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def image_tlvs(data: bytes) -> tuple[tuple[int, bytes, int, int], ...]:
    header = struct.Struct("<IIHHIIBBHI4x")
    if len(data) < header.size:
        raise ValueError("signed binary is shorter than its MCUboot header")
    magic, _, hdr, protected, payload, _, _, _, _, _ = header.unpack_from(data)
    if magic != 0x96F3B83D:
        raise ValueError("signed binary has bad MCUboot magic")
    cursor = hdr + payload + protected
    if cursor + 4 > len(data):
        raise ValueError("signed binary has no TLV area")
    magic_tlv, total = struct.unpack_from("<HH", data, cursor)
    if magic_tlv != 0x6907 or cursor + total > len(data):
        raise ValueError("signed binary has an invalid TLV area")
    end = cursor + total
    cursor += 4
    entries = []
    while cursor < end:
        tlv_type, length = struct.unpack_from("<BxH", data, cursor)
        value_start = cursor + 4
        cursor = value_start + length
        if cursor > end:
            raise ValueError("signed binary has a truncated TLV")
        entries.append((tlv_type, data[value_start:cursor], value_start, cursor))
    return tuple(entries)


def canonical_signed_image(path: pathlib.Path) -> bytes:
    """Remove only randomized signature values from an otherwise exact image."""
    data = bytearray(path.read_bytes())
    for tlv_type, _, start, end in image_tlvs(data):
        if tlv_type in SIGNATURE_TLVS:
            data[start:end] = b"\x00" * (end - start)
    return bytes(data)


def canonical_uf2(
    path: pathlib.Path, signed_image: pathlib.Path, image_base: int = 0x32000
) -> bytes:
    """Remove corresponding randomized signature bytes from a combined UF2."""
    signed_data = signed_image.read_bytes()
    ranges = [
        (image_base + start, image_base + end)
        for tlv_type, _, start, end in image_tlvs(signed_data)
        if tlv_type in SIGNATURE_TLVS
    ]
    expected_coverage = sum(end - start for start, end in ranges)
    covered = 0
    data = bytearray(path.read_bytes())
    if len(data) % 512:
        raise ValueError("UF2 size is not a whole number of blocks")
    for block in range(0, len(data), 512):
        magic0, magic1, _, target, payload = struct.unpack_from("<IIIII", data, block)
        magic_end = struct.unpack_from("<I", data, block + 508)[0]
        if (magic0, magic1, magic_end) != (
            UF2_MAGIC_START0,
            UF2_MAGIC_START1,
            UF2_MAGIC_END,
        ) or payload > 476:
            raise ValueError("combined image contains an invalid UF2 block")
        for start, end in ranges:
            overlap_start = max(target, start)
            overlap_end = min(target + payload, end)
            if overlap_start < overlap_end:
                first = block + 32 + overlap_start - target
                last = block + 32 + overlap_end - target
                signed_first = overlap_start - image_base
                signed_last = overlap_end - image_base
                if data[first:last] != signed_data[signed_first:signed_last]:
                    raise ValueError("UF2 signature bytes do not match the signed image")
                data[first:last] = b"\x00" * (last - first)
                covered += last - first
    if covered != expected_coverage:
        raise ValueError("UF2 does not contain the complete signed-image signature")
    return bytes(data)


def uf2_address_range(path: pathlib.Path) -> tuple[int, int]:
    data = path.read_bytes()
    if not data or len(data) % 512:
        raise ValueError("UF2 is empty or not a whole number of blocks")
    first = None
    last = None
    for block in range(0, len(data), 512):
        magic0, magic1, _, target, payload = struct.unpack_from("<IIIII", data, block)
        magic_end = struct.unpack_from("<I", data, block + 508)[0]
        if (magic0, magic1, magic_end) != (
            UF2_MAGIC_START0,
            UF2_MAGIC_START1,
            UF2_MAGIC_END,
        ) or payload > 476:
            raise ValueError("combined image contains an invalid UF2 block")
        first = target if first is None else min(first, target)
        last = target + payload if last is None else max(last, target + payload)
    assert first is not None and last is not None
    return first, last


def signed_image_metadata(path: pathlib.Path) -> dict[str, object]:
    data = path.read_bytes()
    header = struct.Struct("<IIHHIIBBHI4x")
    _, _, _, _, _, _, major, minor, revision, build = header.unpack_from(data)
    entries = image_tlvs(data)
    types = [entry[0] for entry in entries]
    if not SIGNATURE_TLVS.intersection(types):
        raise ValueError("MCUboot image is not signed")
    values = {tlv_type: value.hex() for tlv_type, value, _, _ in entries}
    return {
        "version": f"{major}.{minor}.{revision}" + (f"+{build}" if build else ""),
        "sha256": digest(path),
        "size": len(data),
        "image_digest": values.get(0x10),
        "key_hash": values.get(0x01),
        "signature_tlvs": sorted(SIGNATURE_TLVS.intersection(types)),
    }


def build(
    root: pathlib.Path,
    workspace: pathlib.Path,
    directory: pathlib.Path,
    source_date_epoch: str,
    firmware_key: pathlib.Path,
    profile: str,
) -> None:
    env = os.environ.copy()
    env.update(
        {
            "ZEPHYR_WORKSPACE": str(workspace),
            "BUILD_DIR": str(directory),
            "PRISTINE": "always",
            "SOURCE_DATE_EPOCH": source_date_epoch,
            "MCUBOOT_SIGNING_KEY": str(firmware_key),
        }
    )
    subprocess.run(
        [str(root / "scripts/build_firmware.sh"), profile], cwd=root, env=env, check=True
    )


def release_copies(
    fleet: pathlib.Path, provisioning: pathlib.Path
) -> dict[pathlib.Path, str]:
    """Select profile-specific release inputs without mixing OTA and USB roles."""
    return {
        fleet / "thread-ring/zephyr/zephyr.signed.bin": "thread-ring.signed.bin",
        fleet / "combined.uf2": "thread-ring.quick-update.uf2",
        provisioning / "combined.uf2": "thread-ring.combined.uf2",
        fleet / "thread-ring/zephyr/zephyr.map": "thread-ring.map",
        fleet / "thread-ring/zephyr/.config": "thread-ring.config",
        provisioning
        / "thread-ring/zephyr/.config": "thread-ring.provisioning.config",
        provisioning / "mcuboot/zephyr/.config": "mcuboot.config",
        fleet / "thread-ring/zephyr/zephyr.dts": "thread-ring.dts",
        fleet / "size-report.txt": "thread-ring.size-report.txt",
        provisioning
        / "size-report.txt": "thread-ring.provisioning.size-report.txt",
    }


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=pathlib.Path, default=root.parent / "zephyr-ws")
    parser.add_argument("--out", type=pathlib.Path, default=root / "dist")
    parser.add_argument(
        "--build-root", type=pathlib.Path, default=root / "firmware/thread-ring/build-release"
    )
    parser.add_argument(
        "--firmware-key",
        type=pathlib.Path,
        default=(
            pathlib.Path(os.environ["MCUBOOT_SIGNING_KEY"])
            if os.environ.get("MCUBOOT_SIGNING_KEY")
            else None
        ),
    )
    parser.add_argument(
        "--attestation-key",
        type=pathlib.Path,
        default=(
            pathlib.Path(os.environ["RELEASE_SIGNING_KEY"])
            if os.environ.get("RELEASE_SIGNING_KEY")
            else None
        ),
    )
    args = parser.parse_args()

    try:
        info = version_info.derive(root)
        if info["dirty"]:
            raise ValueError("release refused: source tree is dirty")
        if not info["exact_tag"]:
            raise ValueError("release refused: HEAD has no exact vMAJOR.MINOR.PATCH tag")
        if args.firmware_key is None:
            raise ValueError("release refused: --firmware-key is required")
        if args.attestation_key is None:
            raise ValueError("release refused: --attestation-key is required")
        firmware_key = signing.firmware_key_info(args.workspace, args.firmware_key)
        attestation_key = signing.private_file(args.attestation_key, "release attestation key")
        allowed_signers = root / "config/release-signers"
        run(
            [
                "git",
                "-c",
                "gpg.format=ssh",
                "-c",
                f"gpg.ssh.allowedSignersFile={allowed_signers}",
                "verify-tag",
                str(info["exact_tag"]),
            ],
            cwd=root,
        )
        run(
            [
                sys.executable,
                str(root / "scripts/check_workspace.py"),
                "--workspace",
                str(args.workspace),
            ],
            cwd=root,
        )
        run([sys.executable, str(root / "scripts/check_partitions.py")], cwd=root)
        epoch = run(["git", "show", "-s", "--format=%ct", "HEAD"], cwd=root).strip()
        fleet_first = args.build_root / "fleet-a"
        fleet_second = args.build_root / "fleet-b"
        provisioning_first = args.build_root / "provisioning-a"
        provisioning_second = args.build_root / "provisioning-b"
        for directory, profile in (
            (fleet_first, "fleet"),
            (fleet_second, "fleet"),
            (provisioning_first, "provisioning"),
            (provisioning_second, "provisioning"),
        ):
            build(
                root,
                args.workspace,
                directory,
                epoch,
                pathlib.Path(firmware_key["path"]),
                profile,
            )
        deterministic = [
            "thread-ring/zephyr/zephyr.bin",
            "mcuboot/zephyr/zephyr.bin",
        ]
        for profile, first, second in (
            ("fleet", fleet_first, fleet_second),
            ("provisioning", provisioning_first, provisioning_second),
        ):
            for item in deterministic:
                if digest(first / item) != digest(second / item):
                    raise ValueError(
                        "release refused: non-reproducible "
                        f"{profile} artifact {item}"
                    )

        mcuboot_rel = "mcuboot/zephyr/zephyr.bin"
        if digest(fleet_first / mcuboot_rel) != digest(
            provisioning_first / mcuboot_rel
        ):
            raise ValueError("release refused: MCUboot differs between profiles")

        signed_rel = "thread-ring/zephyr/zephyr.signed.bin"
        uf2_rel = "combined.uf2"
        fleet_signed = (fleet_first / signed_rel, fleet_second / signed_rel)
        provisioning_signed = (
            provisioning_first / signed_rel,
            provisioning_second / signed_rel,
        )
        for profile, images, uf2s in (
            (
                "fleet",
                fleet_signed,
                (fleet_first / uf2_rel, fleet_second / uf2_rel),
            ),
            (
                "provisioning",
                provisioning_signed,
                (provisioning_first / uf2_rel, provisioning_second / uf2_rel),
            ),
        ):
            if canonical_signed_image(images[0]) != canonical_signed_image(images[1]):
                raise ValueError(
                    "release refused: "
                    f"{profile} signed images differ outside RSA-PSS signature"
                )
            if canonical_uf2(uf2s[0], images[0]) != canonical_uf2(
                uf2s[1], images[1]
            ):
                raise ValueError(
                    "release refused: "
                    f"{profile} UF2s differ outside RSA-PSS signature"
                )

        imgtool = args.workspace / "bootloader/mcuboot/scripts/imgtool.py"
        imgtool_python = args.workspace / ".venv/bin/python"
        for image in (*fleet_signed, *provisioning_signed):
            run(
                [
                    str(imgtool_python),
                    str(imgtool),
                    "verify",
                    "-k",
                    firmware_key["path"],
                    str(image),
                ],
                cwd=root,
            )

        recovery_start, recovery_end = uf2_address_range(
            provisioning_first / uf2_rel
        )
        quick_start, quick_end = uf2_address_range(fleet_first / uf2_rel)
        for role, start, end in (
            ("recovery", recovery_start, recovery_end),
            ("quick-update", quick_start, quick_end),
        ):
            if (start, end) != (RECOVERY_START, RECOVERY_END):
                raise ValueError(
                    f"release refused: {role} UF2 address range "
                    f"0x{start:x}..0x{end:x} is not the exact safe range "
                    f"0x{RECOVERY_START:x}..0x{RECOVERY_END:x}"
                )
            if end > UF2_BOOTLOADER_APP_END:
                raise ValueError(
                    f"release refused: {role} UF2 exceeds the nice!nano v0.6.0 "
                    f"application-write ceiling 0x{UF2_BOOTLOADER_APP_END:x}"
                )

        metadata = signed_image_metadata(fleet_signed[0])
        recovery_metadata = signed_image_metadata(provisioning_signed[0])
        for profile, profile_metadata in (
            ("fleet", metadata),
            ("provisioning", recovery_metadata),
        ):
            if profile_metadata["version"] != info["version"]:
                raise ValueError(
                    "release refused: "
                    f"{profile} image version {profile_metadata['version']} "
                    f"!= tag {info['version']}"
                )
            if profile_metadata["key_hash"] != firmware_key["public_key_sha256"]:
                raise ValueError(
                    "release refused: "
                    f"{profile} signed image key identity does not match "
                    "the configured project key"
                )
        destination = args.out / str(info["exact_tag"])
        destination.mkdir(parents=True, exist_ok=False)
        for source, name in release_copies(fleet_first, provisioning_first).items():
            shutil.copy2(source, destination / name)
        sums = {path.name: digest(path) for path in sorted(destination.iterdir())}
        (destination / "SHA256SUMS").write_text(
            "".join(f"{value}  {name}\n" for name, value in sums.items())
        )
        provenance = {
            "schema": 1,
            "source": info,
            "source_date_epoch": int(epoch),
            "tag_signature_verified": True,
            "image": metadata,
            "recovery_uf2": {
                "start": recovery_start,
                "end_exclusive": recovery_end,
                "preserves_uf2_bootloader_at": 0xF4000,
                "image": recovery_metadata,
            },
            "quick_update_uf2": {
                "start": quick_start,
                "end_exclusive": quick_end,
                "preserves_uf2_bootloader_at": 0xF4000,
                "image": metadata,
            },
            "profiles": {
                "ota": "fleet",
                "recovery_uf2": "provisioning",
                "quick_update_uf2": "fleet",
            },
            "workspace_lock": json.loads((root / "config/toolchain.lock.json").read_text()),
            "signing_key": {
                "profile": "project-rsa-2048",
                "public_key_sha256": firmware_key["public_key_sha256"],
            },
            "attestation": {
                "format": "sshsig",
                "identity": signing.ATTESTATION_IDENTITY,
                "namespace": signing.ATTESTATION_NAMESPACE,
            },
            "artifacts": sums,
        }
        provenance_path = destination / "provenance.json"
        provenance_path.write_text(json.dumps(provenance, indent=2, sort_keys=True) + "\n")
        signing.sign_and_verify(destination / "SHA256SUMS", attestation_key, allowed_signers)
        signing.sign_and_verify(provenance_path, attestation_key, allowed_signers)
        print(destination)
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"release: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
