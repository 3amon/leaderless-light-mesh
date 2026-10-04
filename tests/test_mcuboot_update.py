import hashlib
import importlib.util
import json
import pathlib
import struct
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
PI_TOOLS = ROOT / "pi-tools"
sys.path.insert(0, str(PI_TOOLS))
SPEC = importlib.util.spec_from_file_location("mcuboot_update", PI_TOOLS / "mcuboot_update.py")
UPDATE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = UPDATE
SPEC.loader.exec_module(UPDATE)


def signed_image(version: tuple[int, int, int, int]) -> bytes:
    payload = b"test-image"
    header = struct.pack(
        "<IIHHIIBBHI4x",
        UPDATE.deploy.MCUBOOT_MAGIC,
        0,
        32,
        0,
        len(payload),
        0,
        *version,
    )
    image_hash = hashlib.sha256(header + payload).digest()
    tlv = struct.pack("<HH", UPDATE.deploy.TLV_INFO_MAGIC, 40)
    tlv += struct.pack("<BxH", UPDATE.deploy.TLV_SHA256, 32) + image_hash
    return header + payload + tlv


def write_uf2(path: pathlib.Path, image: bytes) -> None:
    contents = bytearray(b"\xff" * (UPDATE.RECOVERY_END - UPDATE.RECOVERY_START))
    offset = UPDATE.SLOT0_START - UPDATE.RECOVERY_START
    contents[offset : offset + len(image)] = image
    blocks = len(contents) // 256
    output = bytearray()
    for index in range(blocks):
        block = bytearray(512)
        struct.pack_into(
            "<IIIIIIII",
            block,
            0,
            UPDATE.UF2_MAGIC_START0,
            UPDATE.UF2_MAGIC_START1,
            0x2000,
            UPDATE.RECOVERY_START + index * 256,
            256,
            index,
            blocks,
            UPDATE.UF2_FAMILY_NRF52840,
        )
        block[32:288] = contents[index * 256 : (index + 1) * 256]
        struct.pack_into("<I", block, 508, UPDATE.UF2_MAGIC_END)
        output.extend(block)
    path.write_bytes(output)


class McubootUpdateProfileTests(unittest.TestCase):
    def profile(self, root: pathlib.Path) -> pathlib.Path:
        recovery_image = signed_image((0, 1, 44, 0))
        forward_image = signed_image((0, 1, 45, 0))
        rollback_image = signed_image((0, 1, 46, 0))
        recovery = root / "recovery.uf2"
        forward = root / "forward.bin"
        rollback = root / "rollback.bin"
        write_uf2(recovery, recovery_image)
        forward.write_bytes(forward_image)
        rollback.write_bytes(rollback_image)
        recovery_metadata = UPDATE.deploy.inspect_image_bytes(recovery_image, recovery)
        forward_metadata = UPDATE.deploy.inspect_image(forward)
        rollback_metadata = UPDATE.deploy.inspect_image(rollback)
        profile = root / "profile.json"
        profile.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "recovery": {
                        "artifact": recovery.name,
                        "file_sha256": UPDATE.file_sha256(recovery),
                        "signed_file_sha256": recovery_metadata.file_sha256,
                        "version": str(recovery_metadata.version),
                        "image_hash": recovery_metadata.image_hash,
                        "start": "0x26000",
                        "end_exclusive": "0x90000",
                    },
                    "forward": {
                        "artifact": forward.name,
                        "file_sha256": forward_metadata.file_sha256,
                        "version": str(forward_metadata.version),
                        "image_hash": forward_metadata.image_hash,
                    },
                    "rollback": {
                        "artifact": rollback.name,
                        "file_sha256": rollback_metadata.file_sha256,
                        "version": str(rollback_metadata.version),
                        "image_hash": rollback_metadata.image_hash,
                    },
                }
            )
        )
        return profile

    def test_profile_binds_recovery_contents_and_both_ota_images(self):
        with tempfile.TemporaryDirectory() as directory:
            loaded = UPDATE.load_profile(self.profile(pathlib.Path(directory)))
        self.assertEqual((loaded.recovery.start, loaded.recovery.end_exclusive), (0x26000, 0x90000))
        self.assertEqual(str(loaded.recovery.image.version), "0.1.44")
        self.assertEqual(str(loaded.forward.version), "0.1.45")
        self.assertEqual(str(loaded.rollback.version), "0.1.46")

    def test_profile_rejects_unsafe_recovery_range(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = self.profile(pathlib.Path(directory))
            raw = json.loads(profile.read_text())
            raw["recovery"]["end_exclusive"] = "0xec000"
            profile.write_text(json.dumps(raw))
            with self.assertRaisesRegex(ValueError, "must be exactly"):
                UPDATE.load_profile(profile)

    def test_profile_rejects_wrong_embedded_image_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = self.profile(pathlib.Path(directory))
            raw = json.loads(profile.read_text())
            raw["recovery"]["image_hash"] = "AA" * 32
            profile.write_text(json.dumps(raw))
            with self.assertRaisesRegex(ValueError, "embedded recovery image hash"):
                UPDATE.load_profile(profile)

    def test_rollback_result_requires_observed_unconfirmed_candidate(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            result = root / "result.json"
            events = root / "events.jsonl"
            result.write_text(
                json.dumps(
                    {
                        "exit_code": 1,
                        "results": [
                            {
                                "classification": "rollback",
                                "ambiguous": False,
                            }
                        ],
                    }
                )
            )
            rollback = UPDATE.deploy.ImageMetadata(
                root / "rollback.bin",
                UPDATE.deploy.Version(0, 1, 46),
                "AA" * 32,
                "bb" * 32,
                1,
                32,
                1,
            )
            events.write_text(
                json.dumps(
                    {
                        "event": "candidate_active_unconfirmed",
                        "active": {"hash": rollback.image_hash},
                    }
                )
                + "\n"
            )
            UPDATE.validate_rollback_result(result, events, rollback)
            events.write_text("")
            with self.assertRaisesRegex(RuntimeError, "not observed"):
                UPDATE.validate_rollback_result(result, events, rollback)

    def test_disruptive_acknowledgement_is_required_before_profile_access(self):
        with self.assertRaises(SystemExit):
            UPDATE.parse_args(
                [
                    "--profile",
                    "/does/not/exist",
                    "--target",
                    "opal",
                    "--sentinel",
                    "feral",
                    "--out",
                    "/tmp/evidence",
                ]
            )


if __name__ == "__main__":
    unittest.main()
