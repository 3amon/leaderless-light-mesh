import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "check_firmware_size.py"
SPEC = importlib.util.spec_from_file_location("check_firmware_size", SCRIPT)
SIZE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(SIZE)


class FirmwareSizeTests(unittest.TestCase):
    def test_parses_gnu_size_output(self):
        output = """   text\t   data\t    bss\t    dec\t    hex\tfilename
 364244\t   4360\t 113865\t 482469\t  75ca5\tzephyr.elf
"""
        self.assertEqual(SIZE.parse_size_output(output), (364244, 4360, 113865))

    def test_flash_and_ram_accounting(self):
        text, data, bss = (364244, 4360, 113865)
        self.assertAlmostEqual(
            SIZE.percent(text + data, SIZE.DEFAULT_FLASH_CAPACITY), 98.89, places=2
        )
        self.assertAlmostEqual(SIZE.percent(data + bss, SIZE.DEFAULT_RAM_CAPACITY), 45.10, places=2)

    def test_rejects_invalid_capacity(self):
        with self.assertRaises(ValueError):
            SIZE.percent(1, 0)


if __name__ == "__main__":
    unittest.main()
