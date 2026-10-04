import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "firmware" / "thread-ring" / "src"


class LedBudgetTests(unittest.TestCase):
    def test_host_frame_power_cap(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "led_budget_test"
            subprocess.run(
                [
                    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    f"-I{SOURCE_DIR}",
                    str(SOURCE_DIR / "led_budget.c"),
                    str(ROOT / "tests" / "c" / "led_budget_test.c"),
                    "-o", str(executable),
                ],
                check=True,
                cwd=ROOT,
            )
            completed = subprocess.run(
                [str(executable)], check=True, cwd=ROOT, text=True, capture_output=True
            )
            self.assertEqual(completed.stdout.strip(), "led budget tests passed")


if __name__ == "__main__":
    unittest.main()
