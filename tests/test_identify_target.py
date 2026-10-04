import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "identify_target", ROOT / "pi-tools" / "identify_target.py"
)
IDENTIFY = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = IDENTIFY
SPEC.loader.exec_module(IDENTIFY)


class IdentifyTargetTest(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tempdir.name)
        self.serial_dir = self.root / "by-id"
        self.serial_dir.mkdir()
        self.inventory = self.root / "rings.json"
        self.inventory.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "devices": [
                        {
                            "name": "molten-dune-glow",
                            "address": "fd00::1234",
                            "usb_serial": "DEC0DE0000000014",
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )

    def tearDown(self):
        self.tempdir.cleanup()

    def add_by_id(self, name, target="../../ttyACM0"):
        (self.serial_dir / name).symlink_to(target)

    def test_identifies_inventoried_application(self):
        self.add_by_id("usb-ZEPHYR_thread-led-node_thread-ring_DEC0DE0000000014-if00")
        targets = IDENTIFY.identify_targets(
            self.inventory,
            self.serial_dir,
            {"molten-dune-glow": "0.1.80"},
        )
        self.assertEqual(len(targets), 1)
        self.assertEqual(targets[0].name, "molten-dune-glow")
        self.assertEqual(targets[0].mode, "app")
        self.assertEqual(targets[0].firmware_version, "0.1.80")
        self.assertEqual(targets[0].usb_serial, "DEC0DE0000000014")
        self.assertIn("fw=0.1.80", IDENTIFY.format_target(targets[0]))

    def test_identifies_inventoried_dfu_bootloader(self):
        self.add_by_id("usb-Nice_Keyboards_nice_nano_DEC0DE0000000014-if00")
        targets = IDENTIFY.identify_targets(self.inventory, self.serial_dir)
        self.assertEqual(len(targets), 1)
        self.assertEqual(targets[0].name, "molten-dune-glow")
        self.assertEqual(targets[0].mode, "dfu")
        self.assertIsNone(targets[0].firmware_version)
        self.assertIn("fw=unknown", IDENTIFY.format_target(targets[0]))

    def test_reads_versions_from_dashboard_nodes(self):
        class Response:
            def __enter__(self):
                return self

            def __exit__(self, *args):
                return None

            def read(self):
                return json.dumps(
                    {
                        "nodes": {
                            "fd00::1234": {
                                "name": "molten-dune-glow",
                                "img": "0.1.80",
                            },
                            "fd00::5678": {"name": "old-ring"},
                        }
                    }
                ).encode()

        original_urlopen = IDENTIFY.urllib.request.urlopen
        self.addCleanup(setattr, IDENTIFY.urllib.request, "urlopen", original_urlopen)
        IDENTIFY.urllib.request.urlopen = lambda url, timeout: Response()

        self.assertEqual(
            IDENTIFY.load_firmware_versions("http://dashboard/api/state"),
            {"molten-dune-glow": "0.1.80"},
        )

    def test_ignores_unrelated_thread_rcp(self):
        self.add_by_id("usb-Zephyr_Project_Thread_RCP_DEC0DE00000000FF-if00")
        self.assertEqual(IDENTIFY.identify_targets(self.inventory, self.serial_dir), [])

    def test_reports_unknown_target(self):
        self.add_by_id("usb-ZEPHYR_thread-led-node_thread-ring_DEADBEEFDEADBEEF-if00")
        targets = IDENTIFY.identify_targets(self.inventory, self.serial_dir)
        self.assertEqual(len(targets), 1)
        self.assertIsNone(targets[0].name)
        self.assertEqual(targets[0].usb_serial, "DEADBEEFDEADBEEF")

    def test_rejects_duplicate_inventory_serials(self):
        self.inventory.write_text(
            json.dumps(
                {
                    "devices": [
                        {"name": "one", "usb_serial": "SAME"},
                        {"name": "two", "usb_serial": "SAME"},
                    ]
                }
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(ValueError, "duplicate inventory USB serial"):
            IDENTIFY.identify_targets(self.inventory, self.serial_dir)


if __name__ == "__main__":
    unittest.main()
