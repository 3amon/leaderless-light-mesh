import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
PI_TOOLS = ROOT / "pi-tools"
sys.path.insert(0, str(PI_TOOLS))
SPEC = importlib.util.spec_from_file_location("partition_test", PI_TOOLS / "partition_test.py")
PARTITION = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = PARTITION
SPEC.loader.exec_module(PARTITION)


class PartitionTests(unittest.TestCase):
    def image(self, root: pathlib.Path):
        return PARTITION.deploy.ImageMetadata(
            (root / "partition.signed.bin").resolve(),
            PARTITION.deploy.Version(0, 1, 23),
            "CC" * 32,
            "dd" * 32,
            123,
            32,
            80,
        )

    def files(self, root: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path]:
        inventory = root / "rings.json"
        inventory.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "devices": [
                        {
                            "name": "opal",
                            "address": "fd00::1",
                            "usb_serial": "OPAL",
                            "canary": True,
                            "rollout_order": 0,
                        },
                        {
                            "name": "feral",
                            "address": "fd00::2",
                            "usb_serial": "FERAL",
                            "canary": False,
                            "rollout_order": 1,
                        },
                    ],
                }
            )
        )
        profile = root / "partition.json"
        profile.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "target": "opal",
                    "sentinel": "feral",
                    "image": {
                        "artifact": "partition.signed.bin",
                        "file_sha256": "DD" * 32,
                        "version": "0.1.23",
                        "image_hash": "CC" * 32,
                    },
                    "duration_seconds": 30,
                    "rejoin_timeout_seconds": 90,
                }
            )
        )
        return inventory, profile

    def test_profile_binds_usb_canary_and_exact_image(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            inventory, profile_path = self.files(root)
            with mock.patch.object(
                PARTITION.deploy, "inspect_image", return_value=self.image(root)
            ):
                profile = PARTITION.load_profile(profile_path, inventory)
            self.assertEqual(profile.target.name, "opal")
            self.assertEqual(profile.sentinel.name, "feral")
            self.assertEqual(profile.duration_seconds, 30)

    def test_profile_rejects_invalid_fault_duration(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            inventory, profile_path = self.files(root)
            raw = json.loads(profile_path.read_text())
            raw["duration_seconds"] = 301
            profile_path.write_text(json.dumps(raw))
            with self.assertRaisesRegex(ValueError, "duration"):
                PARTITION.load_profile(profile_path, inventory)

    def test_active_image_requires_exact_confirmed_nonpending_slot(self):
        with tempfile.TemporaryDirectory() as directory:
            image = self.image(pathlib.Path(directory))
            states = [
                {
                    "active": True,
                    "version": "0.1.23",
                    "hash": "CC" * 32,
                    "confirmed": True,
                    "pending": False,
                },
                {
                    "active": False,
                    "version": "0.1.22",
                    "hash": "BB" * 32,
                    "confirmed": False,
                    "pending": False,
                },
            ]
            self.assertTrue(PARTITION.active_image_ok(states, image))
            states[1]["pending"] = True
            self.assertFalse(PARTITION.active_image_ok(states, image))

    def test_convergence_requires_identity_infrastructure_and_fresh_peer(self):
        healthy = {"name": "opal", "infra": 1, "nbr": "1/1/1"}
        self.assertTrue(PARTITION.converged(healthy, "opal"))
        self.assertTrue(PARTITION.converged(healthy | {"nbr": "0/1/1"}, "opal"))
        self.assertFalse(PARTITION.converged(healthy | {"infra": 0}, "opal"))
        self.assertFalse(PARTITION.converged(healthy | {"nbr": "0/0/1"}, "opal"))
        self.assertFalse(PARTITION.converged(healthy | {"nbr": "malformed"}, "opal"))


if __name__ == "__main__":
    unittest.main()
