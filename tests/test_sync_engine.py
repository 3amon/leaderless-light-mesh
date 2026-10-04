import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "firmware" / "thread-ring" / "src"


class SyncEngineTests(unittest.TestCase):
    def test_engine_core(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "sync_engine_test"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    f"-I{SOURCE_DIR}",
                    str(SOURCE_DIR / "sync_engine.c"),
                    str(SOURCE_DIR / "clock_estimator.c"),
                    str(SOURCE_DIR / "peer_freshness.c"),
                    str(SOURCE_DIR / "peer_slot_policy.c"),
                    str(ROOT / "tests" / "c" / "sync_engine_test.c"),
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
            self.assertEqual(completed.stdout.strip(), "sync engine tests passed")


if __name__ == "__main__":
    unittest.main()
