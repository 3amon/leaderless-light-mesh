import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "firmware" / "thread-ring" / "src"
PATCH = ROOT / "patches" / "mcuboot-watchdog-trace.patch"
OVERLAYS = [
    ROOT / "firmware" / "thread-ring" / "boards" / "promicro_nrf52840_nrf52840_uf2.overlay",
    ROOT / "firmware" / "thread-ring" / "boards" / "promicro_nrf52840_nrf52840.overlay",
    ROOT / "firmware" / "thread-ring" / "sysbuild" / "mcuboot.overlay",
]


class EscapeLadderTests(unittest.TestCase):
    def test_host_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "escape_ladder_test"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    f"-I{SOURCE_DIR}",
                    str(ROOT / "tests" / "c" / "escape_ladder_test.c"),
                    "-o",
                    str(executable),
                ],
                check=True,
                cwd=ROOT,
            )
            completed = subprocess.run(
                [str(executable)],
                check=True,
                cwd=ROOT,
                text=True,
                capture_output=True,
            )
            self.assertEqual(completed.stdout.strip(), "escape ladder tests passed")

    def test_mcuboot_patch_carries_identical_header_copy(self):
        """The ladder is compiled in two separate images; the copies must not drift.

        The MCUboot patch adds boot/zephyr/include/escape_ladder.h as a new
        file, so every line of that file appears as a '+' line in exactly one
        diff hunk. Reconstruct it and compare byte-for-byte with the
        application's header.
        """
        patch_text = PATCH.read_text()
        marker = "+++ b/boot/zephyr/include/escape_ladder.h\n"
        self.assertIn(marker, patch_text, "patch no longer adds the shared header")
        section = patch_text.split(marker, 1)[1]
        embedded_lines = []
        for line in section.splitlines(keepends=True):
            if line.startswith("@@"):
                continue
            if line.startswith("+"):
                embedded_lines.append(line[1:])
                continue
            break  # first non-added line ends the new-file hunk
        embedded = "".join(embedded_lines)
        source = (SOURCE_DIR / "escape_ladder.h").read_text()
        self.assertEqual(embedded, source)

    def test_all_images_carve_the_ladder_out_of_sram(self):
        """MCUboot and both application board variants must agree on the ABI."""
        address = re.search(
            r"#define ESCAPE_LADDER_RAM_ADDR (0x[0-9a-f]+)UL",
            (SOURCE_DIR / "escape_ladder.h").read_text(),
        )
        self.assertIsNotNone(address)
        self.assertEqual(int(address.group(1), 16), 0x2003D000)
        for overlay in OVERLAYS:
            text = overlay.read_text()
            self.assertIn(
                "reg = <0x20000000 0x3d000>;",
                text,
                f"{overlay.name} does not reserve the escape-ladder RAM",
            )


if __name__ == "__main__":
    unittest.main()
