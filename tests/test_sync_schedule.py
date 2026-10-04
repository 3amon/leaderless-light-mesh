import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "firmware" / "thread-ring" / "src"


class SyncScheduleTests(unittest.TestCase):
    def test_schedule_and_scale_simulation(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "sync_schedule_test"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    f"-I{SOURCE_DIR}",
                    str(SOURCE_DIR / "sync_schedule.c"),
                    str(SOURCE_DIR / "group_policy.c"),
                    str(ROOT / "tests" / "c" / "sync_schedule_test.c"),
                    "-o",
                    str(executable),
                ],
                check=True,
                cwd=ROOT,
            )
            completed = subprocess.run(
                [str(executable)], check=True, cwd=ROOT, text=True, capture_output=True
            )
            self.assertIn("9 nodes converged", completed.stdout)
            self.assertIn("24 nodes converged", completed.stdout)
            self.assertIn("sync schedule simulations passed", completed.stdout)


if __name__ == "__main__":
    unittest.main()
