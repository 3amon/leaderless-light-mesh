import os
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "pi-tools"))

import control_client as CONTROL  # noqa: E402


class ControlClientTests(unittest.TestCase):
    def key_file(self, root: pathlib.Path, text: str | None = None) -> pathlib.Path:
        path = root / "opal-reef-tide.key"
        path.write_text(text or bytes(range(32)).hex() + "\n")
        path.chmod(0o600)
        return path

    def test_known_authenticated_wire_vector_fits_longest_command(self):
        datagram = CONTROL.build_authenticated("scramble -600000", 42, bytes(range(32)))
        self.assertEqual(
            datagram,
            b"v2 000000000000002a scramble -600000 955bb2d1234e91c4e1d5932e",
        )
        self.assertEqual(len(datagram), 61)

    def test_rejects_noncanonical_read_only_and_oversized_commands(self):
        key = bytes(range(32))
        for command in ("status", "thresh", " reboot", "reboot ", "flash  200", "réboot"):
            with self.subTest(command=command), self.assertRaises(CONTROL.ControlAuthError):
                CONTROL.build_authenticated(command, 1, key)
        with self.assertRaisesRegex(CONTROL.ControlAuthError, "exceeds"):
            CONTROL.build_authenticated(
                "scramble -6000000000000000000000000000000000000000000000", 1, key
            )

    def test_removed_threshold_commands_are_not_mutations(self):
        self.assertFalse(CONTROL.command_is_mutating("thresh"))
        self.assertFalse(CONTROL.command_is_mutating("thresh -70 -80"))

    def test_layout_query_is_read_only_but_changes_are_authenticated(self):
        self.assertFalse(CONTROL.command_is_mutating("layout"))
        self.assertTrue(CONTROL.command_is_mutating("layout jewel7"))
        self.assertTrue(CONTROL.command_is_mutating("layout ring16"))
        self.assertTrue(CONTROL.command_is_mutating("layout rope100"))
        datagram = CONTROL.build_authenticated("layout jewel7", 42, bytes(range(32)))
        self.assertLessEqual(len(datagram), CONTROL.MAX_DATAGRAM)

    def test_power_query_is_read_only_but_changes_are_authenticated(self):
        self.assertFalse(CONTROL.command_is_mutating("power"))
        self.assertTrue(CONTROL.command_is_mutating("power carrier"))
        self.assertTrue(CONTROL.command_is_mutating("power module-vcc"))
        datagram = CONTROL.build_authenticated("power module-vcc", 42, bytes(range(32)))
        self.assertLessEqual(len(datagram), CONTROL.MAX_DATAGRAM)

    def test_sleep_is_authenticated(self):
        self.assertTrue(CONTROL.command_is_mutating("sleep"))
        datagram = CONTROL.build_authenticated("sleep", 42, bytes(range(32)))
        self.assertLessEqual(len(datagram), CONTROL.MAX_DATAGRAM)

    def test_quiet_query_is_read_only_but_changes_are_authenticated(self):
        self.assertFalse(CONTROL.command_is_mutating("quiet"))
        self.assertTrue(CONTROL.command_is_mutating("quiet 1800"))
        self.assertTrue(CONTROL.command_is_mutating("quiet off"))
        datagram = CONTROL.build_authenticated("quiet 14400", 42, bytes(range(32)))
        self.assertLessEqual(len(datagram), CONTROL.MAX_DATAGRAM)

    def test_mode_query_is_read_only_but_explicit_priority_changes_are_authenticated(self):
        self.assertFalse(CONTROL.command_is_mutating("mode"))
        self.assertTrue(CONTROL.command_is_mutating("mode party 10"))
        self.assertTrue(CONTROL.command_is_mutating("mode normal 20"))
        datagram = CONTROL.build_authenticated("mode normal 4294967295", 42, bytes(range(32)))
        self.assertLessEqual(len(datagram), CONTROL.MAX_DATAGRAM)

    def test_key_file_must_be_owned_regular_lowercase_hex_and_private(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            path = self.key_file(root)
            self.assertEqual(CONTROL.load_key(path), bytes(range(32)))
            path.chmod(0o640)
            with self.assertRaisesRegex(CONTROL.ControlAuthError, "group/other"):
                CONTROL.load_key(path)
            path.chmod(0o600)
            path.write_text(bytes(range(32)).hex().upper())
            with self.assertRaisesRegex(CONTROL.ControlAuthError, "lowercase"):
                CONTROL.load_key(path)
            path.unlink()
            target = root / "target"
            target.write_text(bytes(range(32)).hex())
            os.symlink(target, path)
            with self.assertRaisesRegex(CONTROL.ControlAuthError, "non-symlink"):
                CONTROL.load_key(path)

    def test_status_and_response_parsing_are_exact_and_correlated(self):
        self.assertEqual(CONTROL.parse_auth_status("auth=ready seq=000000000000002a"), (True, 42))
        self.assertEqual(
            CONTROL.parse_auth_status("auth=unprovisioned seq=0000000000000000"),
            (False, 0),
        )
        self.assertEqual(
            CONTROL.parse_authenticated_response(
                "v2 000000000000002a ok rebooting 57e016b699b54a638ceacffe",
                42,
                bytes(range(32)),
            ),
            "rebooting",
        )
        with self.assertRaisesRegex(CONTROL.ControlAuthError, "mismatch"):
            CONTROL.parse_authenticated_response(
                "v2 000000000000002b ok rebooting 000000000000000000000000",
                42,
                bytes(range(32)),
            )
        with self.assertRaisesRegex(CONTROL.ControlAuthError, "rejected"):
            CONTROL.parse_authenticated_response(
                "v2 000000000000002a error replay aff3d39e26f4bacdb6859eca",
                42,
                bytes(range(32)),
            )
        with self.assertRaisesRegex(CONTROL.ControlAuthError, "authentication failed"):
            CONTROL.parse_authenticated_response(
                "v2 000000000000002a ok rebooting 000000000000000000000000",
                42,
                bytes(range(32)),
            )
        with self.assertRaisesRegex(CONTROL.ControlAuthReplyMissing, "did not return"):
            CONTROL.parse_authenticated_response(None, 42, bytes(range(32)))

    def test_authenticated_command_queries_sequence_under_device_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            key_path = self.key_file(root)
            calls = []

            def exchange(address, datagram, timeout):
                calls.append((address, datagram, timeout))
                if datagram == b"auth":
                    return "auth=ready seq=0000000000000029"
                return "v2 000000000000002a ok bright=45 eae1c62f88d8579564b97ab4"

            result = CONTROL.authenticated_udp_command(
                "fd00::1",
                "opal-reef-tide",
                "bright 45",
                0.5,
                key_path=key_path,
                lock_dir=root / "locks",
                exchange=exchange,
            )
            self.assertEqual(result, "bright=45")
            self.assertEqual(calls[0][1], b"auth")
            self.assertTrue(calls[1][1].startswith(b"v2 000000000000002a bright 45 "))

    def test_unprovisioned_device_never_receives_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            key_path = self.key_file(root)
            exchange = mock.Mock(return_value="auth=unprovisioned seq=0000000000000000")
            with self.assertRaisesRegex(CONTROL.ControlAuthError, "no provisioned"):
                CONTROL.authenticated_udp_command(
                    "fd00::1",
                    "opal-reef-tide",
                    "reboot",
                    0.5,
                    key_path=key_path,
                    lock_dir=root / "locks",
                    exchange=exchange,
                )
            exchange.assert_called_once_with("fd00::1", b"auth", 0.5)


if __name__ == "__main__":
    unittest.main()
