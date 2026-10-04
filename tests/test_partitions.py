import pathlib
import re
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts"))
import check_partitions
import release


class PartitionTests(unittest.TestCase):
    def test_real_maps_are_identical_and_contiguous(self):
        root = pathlib.Path(__file__).resolve().parents[1]
        app = check_partitions.partitions(
            root / "firmware/thread-ring/boards/promicro_nrf52840_nrf52840_uf2.overlay"
        )
        boot = check_partitions.partitions(root / "firmware/thread-ring/sysbuild/mcuboot.overlay")
        self.assertEqual(app, boot)
        self.assertEqual(app["image-0"], (0x32000, 0x5D000))
        self.assertEqual(app["storage"], (0xEC000, 0x8000))

    def test_recovery_uf2_invalidates_secondary_within_native_bootloader_limit(self):
        root = pathlib.Path(__file__).resolve().parents[1]
        app = check_partitions.partitions(
            root / "firmware/thread-ring/boards/promicro_nrf52840_nrf52840_uf2.overlay"
        )
        source = (root / "firmware/thread-ring/make_ota_uf2.sh").read_text()
        match = re.search(r"^lo, hi = (0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+)$", source, re.MULTILINE)

        self.assertIsNotNone(match)
        recovery_range = tuple(int(value, 16) for value in match.groups())
        secondary_start = app["image-1"][0]
        self.assertEqual(recovery_range, (app["mcuboot"][0], secondary_start + 0x1000))
        self.assertEqual(recovery_range, (release.RECOVERY_START, release.RECOVERY_END))
        self.assertLessEqual(recovery_range[1], release.UF2_BOOTLOADER_APP_END)
        self.assertLess(recovery_range[1], app["storage"][0])

    def test_empty_map_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "empty.overlay"
            path.write_text("/ { };\n")
            with self.assertRaises(ValueError):
                check_partitions.partitions(path)
