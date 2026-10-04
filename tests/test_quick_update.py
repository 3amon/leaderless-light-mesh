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
SCRIPT = PI_TOOLS / "quick_update.py"
SPEC = importlib.util.spec_from_file_location("quick_update", SCRIPT)
QUICK = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(QUICK)


def inventory(path: pathlib.Path) -> None:
    path.write_text(
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


class QuickUpdateTests(unittest.TestCase):
    def test_auto_detects_exactly_one_inventoried_usb_node(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "rings.json"
            inventory(path)
            with (
                mock.patch.object(
                    QUICK.uf2,
                    "usb_mode",
                    side_effect=lambda serial: "app" if serial == "FERAL" else "absent",
                ),
                mock.patch.object(QUICK.uf2, "block_device_for_serial", return_value=None),
            ):
                device = QUICK.connected_device(path)
            self.assertEqual(device.name, "feral")
            self.assertEqual(device.usb_serial, "FERAL")

    def test_rejects_multiple_connected_inventory_nodes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "rings.json"
            inventory(path)
            with (
                mock.patch.object(QUICK.uf2, "usb_mode", return_value="app"),
                mock.patch.object(QUICK.uf2, "block_device_for_serial", return_value=None),
                self.assertRaisesRegex(ValueError, "exactly one"),
            ):
                QUICK.connected_device(path)

    def test_explicit_target_still_requires_its_usb_serial(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "rings.json"
            inventory(path)
            with (
                mock.patch.object(QUICK.uf2, "usb_mode", return_value="absent"),
                mock.patch.object(QUICK.uf2, "block_device_for_serial", return_value=None),
                self.assertRaisesRegex(ValueError, "no inventoried USB node"),
            ):
                QUICK.connected_device(path, "feral")

    def test_wait_for_application_requires_exact_serial_app_mode(self):
        args = mock.Mock(usb_serial="EXACT", return_seconds=1, poll_interval=0)
        evidence = mock.Mock()
        with (
            mock.patch.object(QUICK.uf2, "usb_mode", side_effect=["absent", "app"]),
            mock.patch.object(QUICK.boot, "usb_path", return_value="/dev/ttyACM7"),
        ):
            elapsed = QUICK.wait_for_application_usb(args, evidence)
        self.assertGreaterEqual(elapsed, 0)


if __name__ == "__main__":
    unittest.main()
