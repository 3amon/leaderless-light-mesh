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
SCRIPT = PI_TOOLS / "uf2_recovery_test.py"
SPEC = importlib.util.spec_from_file_location("uf2_recovery_test", SCRIPT)
UF2 = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(UF2)


class Uf2RecoveryTests(unittest.TestCase):
    def test_usb_mode_prefers_target_bootloader_when_another_app_is_present(self):
        mixed_output = (
            f"Bus 001 Device 010: ID {UF2.APP_USB_ID} ring\n"
            f"Bus 001 Device 011: ID {UF2.BOOTLOADER_USB_ID} nice!nano\n"
        )
        with mock.patch.object(
            UF2,
            "run",
            return_value=mock.Mock(stdout=mixed_output),
        ):
            self.assertEqual(UF2.usb_mode(), "bootloader")

    def test_usb_mode_resolves_exact_serial_identity(self):
        properties = "ID_VENDOR_ID=239a\nID_MODEL_ID=00b3\n"
        with (
            mock.patch.object(UF2.boot, "usb_path", return_value="/dev/ttyACM7"),
            mock.patch.object(
                UF2,
                "run",
                return_value=mock.Mock(stdout=properties),
            ) as run,
        ):
            self.assertEqual(UF2.usb_mode("EXACT"), "bootloader")
        run.assert_called_once_with(
            ["udevadm", "info", "--query=property", "--name", "/dev/ttyACM7"]
        )

    def test_block_device_resolves_exact_serial_not_device_position(self):
        output = json.dumps(
            {
                "blockdevices": [
                    {
                        "path": "/dev/sda",
                        "label": "NICENANO",
                        "serial": "OTHER",
                        "type": "disk",
                        "mountpoints": [None],
                    },
                    {
                        "path": "/dev/sdc",
                        "label": "NICENANO",
                        "serial": "EXACT",
                        "type": "disk",
                        "mountpoints": ["/media/pi/NICENANO"],
                    },
                ]
            }
        )
        with mock.patch.object(UF2, "run", return_value=mock.Mock(returncode=0, stdout=output)):
            self.assertEqual(UF2.block_device_for_serial("EXACT"), "/dev/sdc")

    def test_file_sha256(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "image.uf2"
            path.write_bytes(b"rescue")
            self.assertEqual(
                UF2.file_sha256(path),
                "824ef7874151e9c8c4f7c3c821d0e5da563a40bbfad1675d991aafabecb73139",
            )

    def test_parse_args_supplies_control_auth_options_to_shared_backend(self):
        with tempfile.TemporaryDirectory() as directory:
            artifact = pathlib.Path(directory) / "image.uf2"
            artifact.write_bytes(b"rescue")
            args = UF2.parse_args(
                [
                    "--artifact",
                    str(artifact),
                    "--artifact-sha256",
                    UF2.file_sha256(artifact),
                    "--out",
                    str(pathlib.Path(directory) / "evidence"),
                ]
            )
            self.assertIsNone(args.control_key_dir)
            self.assertFalse(args.allow_legacy_control)

    def test_active_image_gate(self):
        states = [
            {
                "active": True,
                "version": "0.1.10",
                "hash": "AB" * 32,
                "confirmed": True,
                "pending": False,
            }
        ]
        self.assertTrue(UF2.active_image_ok(states, "0.1.10", "ab" * 32))
        self.assertFalse(UF2.active_image_ok(states, "0.1.11", "ab" * 32))

    def test_recovery_class_preserves_watchdog_assist(self):
        self.assertEqual(UF2.recovery_class({"reset": "00000002/uf2"}), "direct")
        self.assertEqual(
            UF2.recovery_class({"reset": "00000010/none"}),
            "watchdog-assisted",
        )
        self.assertIsNone(UF2.recovery_class({"reset": "00000002/command"}))

    def test_inventory_identity_accepts_explicit_usb_target(self):
        with tempfile.TemporaryDirectory() as directory:
            inventory = pathlib.Path(directory) / "rings.json"
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
            UF2.validate_inventory_identity(
                inventory, "feral", "FD00::2", "FERAL", "opal", "fd00::1"
            )

    def test_inventory_identity_rejects_wrong_usb_serial(self):
        with self.assertRaisesRegex(ValueError, "USB serial"):
            UF2.validate_inventory_identity(
                ROOT / "config/rings.json",
                "feral-tide-light",
                "fd00:db8:0:1:a002:b002:c002:d002",
                "DEC0DE0000000022",
                "opal-reef-tide",
                "fd00:db8:0:1:a022:b022:c022:d022",
            )


if __name__ == "__main__":
    unittest.main()
