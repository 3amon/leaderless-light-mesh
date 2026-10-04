import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "firmware" / "thread-ring" / "src"


class ClockEstimatorTests(unittest.TestCase):
    def test_host_trace_replay(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "clock_estimator_test"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    f"-I{SOURCE_DIR}",
                    str(SOURCE_DIR / "clock_estimator.c"),
                    str(ROOT / "tests" / "c" / "clock_estimator_test.c"),
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
            self.assertEqual(completed.stdout.strip(), "clock estimator tests passed")


if __name__ == "__main__":
    unittest.main()
