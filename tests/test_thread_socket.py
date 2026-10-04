import importlib.util
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "thread_socket", ROOT / "pi-tools" / "thread_socket.py"
)
THREAD_SOCKET = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(THREAD_SOCKET)


class ThreadSourceSelectionTests(unittest.TestCase):
    def source(self, rows, remote="fd00:db8:0:1::1234"):
        with tempfile.TemporaryDirectory() as directory:
            table = pathlib.Path(directory) / "if_inet6"
            table.write_text("\n".join(rows))
            return THREAD_SOCKET.stable_source_address(remote, address_table=table)

    def test_prefers_ordinary_omr_address_over_thread_locators(self):
        self.assertEqual(
            self.source(
                [
                    "fd000db800000001000000fffe00fc38 06 40 00 80 wpan0",
                    "fd000db800000001000000fffe000c00 06 40 00 80 wpan0",
                    "fd000db800000001a02ab02ac02ad02a 06 40 00 80 wpan0",
                    "fd000db8000000011111111111111111 03 40 00 80 wlan0",
                ]
            ),
            "fd00:db8:0:1:a02a:b02a:c02a:d02a",
        )

    def test_requires_same_prefix_and_interface(self):
        self.assertIsNone(
            self.source(
                [
                    "fd000db800009999a02ab02ac02ad02a 06 40 00 80 wpan0",
                    "fd000db800000001a02ab02ac02ad02a 03 40 00 80 wlan0",
                ]
            )
        )

    def test_missing_or_invalid_table_falls_back_to_kernel_selection(self):
        missing = pathlib.Path("/definitely/missing/if_inet6")
        self.assertIsNone(
            THREAD_SOCKET.stable_source_address("fd00:db8:0:1::1", address_table=missing)
        )
        self.assertIsNone(THREAD_SOCKET.stable_source_address("not-an-address"))


if __name__ == "__main__":
    unittest.main()
