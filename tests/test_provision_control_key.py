import hashlib
import importlib.util
import json
import pathlib
import stat
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
PI_TOOLS = ROOT / "pi-tools"
sys.path.insert(0, str(PI_TOOLS))
SPEC = importlib.util.spec_from_file_location(
    "provision_control_key", PI_TOOLS / "provision_control_key.py"
)
PROVISION = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(PROVISION)


class ProvisionControlKeyTests(unittest.TestCase):
    def test_shell_prompt_detection_ignores_ansi_decoration(self):
        self.assertTrue(PROVISION.shell_prompt_seen(b"\x1b[1;32muart:~$ \x1b[m"))
        self.assertFalse(PROVISION.shell_prompt_seen(b"application log output"))

    @mock.patch.object(PROVISION.time, "sleep")
    @mock.patch.object(PROVISION.os, "write")
    def test_serial_write_is_paced_below_shell_rx_ring(self, write, sleep):
        chunks = []

        def accept(_descriptor, payload):
            chunks.append(bytes(payload))
            return len(payload)

        write.side_effect = accept
        payload = bytes(range(80))
        PROVISION.write_paced(7, payload)
        self.assertEqual(b"".join(chunks), payload)
        self.assertLessEqual(max(map(len, chunks)), 8)
        self.assertEqual(sleep.call_count, 9)

    def test_inventory_requires_exact_named_usb_identity(self):
        target = PROVISION.inventory_target(ROOT / "config/rings.json", "opal-reef-tide")
        self.assertEqual(target.usb_serial, "DEC0DE0000000022")
        carrier = PROVISION.inventory_target(ROOT / "config/rings.json", "lucid-iris-light")
        self.assertEqual(carrier.usb_serial, "DEC0DE0000000003")
        self.assertEqual(carrier.address, "fd00:db8:0:1:a003:b003:c003:d003")
        with self.assertRaisesRegex(ValueError, "exactly one"):
            PROVISION.inventory_target(ROOT / "config/rings.json", "missing")

    def test_created_key_is_private_and_reused(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "private" / "opal.key"
            key, created = PROVISION.create_or_load_key(path)
            self.assertTrue(created)
            self.assertEqual(len(key), 32)
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE(path.parent.stat().st_mode), 0o700)
            loaded, created_again = PROVISION.create_or_load_key(path)
            self.assertFalse(created_again)
            self.assertEqual(loaded, key)

    def test_existing_ready_device_is_verified_without_usb_write(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            key_path = root / "keys" / "opal.key"
            key_path.parent.mkdir(mode=0o700)
            key = bytes(range(32))
            key_path.write_text(key.hex() + "\n")
            key_path.chmod(0o600)
            target = PROVISION.deploy.Device("opal", "fd00::1", "OPAL", True, 0)
            replies = iter(
                (
                    "auth=ready seq=0000000000000029",
                    "auth=ready seq=0000000000000029",
                    "v2 000000000000002a ok hb-sent 8b6772c07c66b9e5ed55f98c",
                    "auth=ready seq=000000000000002a",
                )
            )
            exchange = mock.Mock(side_effect=lambda *_: next(replies))
            serial = mock.Mock()
            result = PROVISION.provision_target(
                target,
                key_path,
                0.5,
                exchange=exchange,
                serial_provision=serial,
            )
            serial.assert_not_called()
            self.assertFalse(result["serial_written"])
            self.assertEqual(result["key_sha256"], hashlib.sha256(key).hexdigest())
            self.assertNotIn(key.hex(), json.dumps(result))

    def test_unprovisioned_device_uses_exact_serial_then_verifies(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            key_path = root / "keys" / "opal.key"
            target = PROVISION.deploy.Device("opal", "fd00::1", "EXACT", True, 0)
            state = {"sequence": 0, "ready": False}

            def exchange(_address, datagram, _timeout):
                if datagram == b"auth":
                    status = "ready" if state["ready"] else "unprovisioned"
                    return f"auth={status} seq={state['sequence']:016x}"
                state["sequence"] = 1
                prefix = "v2 0000000000000001 ok hb-sent"
                key = PROVISION.control_client.load_key(key_path)
                tag = (
                    __import__("hmac")
                    .new(key, prefix.encode(), __import__("hashlib").sha256)
                    .digest()[:12]
                    .hex()
                )
                return f"{prefix} {tag}"

            def serial(serial_number, _key, _timeout):
                self.assertEqual(serial_number, "EXACT")
                state["ready"] = True

            result = PROVISION.provision_target(
                target,
                key_path,
                0.5,
                exchange=exchange,
                serial_provision=serial,
            )
            self.assertTrue(result["serial_written"])
            self.assertEqual(result["final_sequence"], "0000000000000001")

    def test_private_directory_rejects_open_group_permissions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory) / "keys"
            root.mkdir(mode=0o750)
            with self.assertRaisesRegex(RuntimeError, "group/other"):
                PROVISION.create_or_load_key(root / "opal.key")


if __name__ == "__main__":
    unittest.main()
