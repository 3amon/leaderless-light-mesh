import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "firmware" / "thread-ring" / "src"


class PairBootstrapTests(unittest.TestCase):
    def test_far_apart_pair_locks_and_crowd_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "pair_bootstrap_test"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-O1",
                    "-Wall",
                    "-Wextra",
                    "-pedantic",
                    f"-I{SOURCE_DIR}",
                    f"-I{ROOT / 'tests' / 'c' / 'campaign'}",
                    str(SOURCE_DIR / "group_engine.c"),
                    str(SOURCE_DIR / "group_policy.c"),
                    str(SOURCE_DIR / "social_scene.c"),
                    str(SOURCE_DIR / "peer_slot_policy.c"),
                    str(SOURCE_DIR / "sync_schedule.c"),
                    str(SOURCE_DIR / "clock_estimator.c"),
                    str(ROOT / "tests" / "c" / "pair_bootstrap_test.c"),
                    "-lm",
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
            self.assertEqual(completed.stdout.strip(), "pair bootstrap tests passed")


if __name__ == "__main__":
    unittest.main()
