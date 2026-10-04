import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "firmware" / "thread-ring" / "src"


class GenomeTests(unittest.TestCase):
    def test_host_render_policy(self):
        for led_count in (16, 7, 100):
            with self.subTest(led_count=led_count), tempfile.TemporaryDirectory() as directory:
                executable = Path(directory) / "genome_test"
                subprocess.run(
                    [
                        "cc",
                        "-std=c11",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        "-pedantic",
                        f"-DTEST_LED_COUNT={led_count}",
                        f"-I{SOURCE_DIR}",
                        str(SOURCE_DIR / "genome.c"),
                        str(ROOT / "tests" / "c" / "genome_test.c"),
                        "-lm",
                        "-o",
                        str(executable),
                    ],
                    check=True,
                    cwd=ROOT,
                )
                completed = subprocess.run(
                    [str(executable)], check=True, cwd=ROOT, text=True, capture_output=True
                )
                self.assertEqual(completed.stdout.strip(), "genome tests passed")


if __name__ == "__main__":
    unittest.main()
