import argparse
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
SCRIPT = PI_TOOLS / "rollback_test.py"
SPEC = importlib.util.spec_from_file_location("rollback_test", SCRIPT)
ROLLBACK = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = ROLLBACK
SPEC.loader.exec_module(ROLLBACK)


class RollbackStateTests(unittest.TestCase):
    def setUp(self):
        self.candidate_hash = "30" * 32
        self.args = argparse.Namespace(
            active_version="0.1.10",
            active_hash="BD" * 32,
            candidate_version="0.1.11",
            candidate_hash=self.candidate_hash,
        )
        self.states = [
            {
                "slot": 0,
                "version": "0.1.10",
                "hash": "BD" * 32,
                "pending": False,
                "confirmed": True,
                "active": True,
            },
            {
                "slot": 1,
                "version": "0.1.11",
                "hash": self.candidate_hash,
                "pending": False,
                "confirmed": False,
                "active": False,
            },
        ]

    def test_accepts_exact_typed_slot_state(self):
        ROLLBACK.verify_slots(self.args, self.states)

    def test_rejects_wrong_candidate_hash(self):
        self.states[1]["hash"] = "FF" * 32
        with self.assertRaises(RuntimeError):
            ROLLBACK.verify_slots(self.args, self.states)

    def test_accepts_mcuboot_four_component_candidate_version(self):
        self.args.candidate_version = "0.1.22+1"
        self.states[1]["version"] = "0.1.22.1"
        ROLLBACK.verify_slots(self.args, self.states)

    def test_rejects_wrong_active_hash(self):
        self.states[0]["hash"] = "EE" * 32
        with self.assertRaisesRegex(RuntimeError, "exact confirmed"):
            ROLLBACK.verify_slots(self.args, self.states)


class RollbackProfileTests(unittest.TestCase):
    def write_inventory(self, root: pathlib.Path) -> pathlib.Path:
        inventory = root / "rings.json"
        inventory.write_text(
            """{
  "schema": 1,
  "devices": [
    {"name":"opal", "address":"fd00::1", "usb_serial":"OPAL", "canary":true, "rollout_order":0},
    {"name":"feral", "address":"fd00::2", "usb_serial":"FERAL", "canary":false, "rollout_order":1}
  ]
}\n"""
        )
        return inventory

    def write_profile(self, root: pathlib.Path, **candidate_overrides) -> pathlib.Path:
        candidate = {
            "artifact": "rollback.signed.bin",
            "file_sha256": "DD" * 32,
            "version": "0.1.23",
            "image_hash": "CC" * 32,
        }
        candidate.update(candidate_overrides)
        profile = root / "rollback.json"
        profile.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "target": "opal",
                    "sentinel": "feral",
                    "baseline": {"version": "0.1.22", "image_hash": "BB" * 32},
                    "candidate": candidate,
                    "trials": 5,
                }
            )
        )
        return profile

    def image(self, root: pathlib.Path):
        return ROLLBACK.deploy.ImageMetadata(
            (root / "rollback.signed.bin").resolve(),
            ROLLBACK.deploy.Version(0, 1, 23),
            "CC" * 32,
            "dd" * 32,
            123,
            32,
            80,
        )

    def test_profile_binds_inventory_and_exact_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            inventory = self.write_inventory(root)
            profile_path = self.write_profile(root)
            with mock.patch.object(ROLLBACK.deploy, "inspect_image", return_value=self.image(root)):
                profile = ROLLBACK.load_profile(profile_path, inventory)
            self.assertEqual(profile.target.name, "opal")
            self.assertEqual(profile.sentinel.name, "feral")
            self.assertEqual(profile.artifact.image_hash, "CC" * 32)
            self.assertEqual(profile.trials, 5)

    def test_profile_rejects_file_hash_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            inventory = self.write_inventory(root)
            profile_path = self.write_profile(root, file_sha256="EE" * 32)
            with mock.patch.object(ROLLBACK.deploy, "inspect_image", return_value=self.image(root)):
                with self.assertRaisesRegex(ValueError, "file SHA-256"):
                    ROLLBACK.load_profile(profile_path, inventory)

    def test_profile_rejects_non_canary_target(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            inventory = self.write_inventory(root)
            profile_path = self.write_profile(root)
            raw = json.loads(profile_path.read_text())
            raw["target"], raw["sentinel"] = "feral", "opal"
            profile_path.write_text(json.dumps(raw))
            with mock.patch.object(ROLLBACK.deploy, "inspect_image", return_value=self.image(root)):
                with self.assertRaisesRegex(ValueError, "inventoried canary"):
                    ROLLBACK.load_profile(profile_path, inventory)


if __name__ == "__main__":
    unittest.main()
