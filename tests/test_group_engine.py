import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "firmware" / "thread-ring" / "src"


class GroupEngineTests(unittest.TestCase):
    def test_host_engine_and_mini_simulator(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "group_engine_test"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    f"-I{SOURCE_DIR}",
                    str(SOURCE_DIR / "group_engine.c"),
                    str(SOURCE_DIR / "group_policy.c"),
                    str(SOURCE_DIR / "social_scene.c"),
                    str(ROOT / "tests" / "c" / "group_engine_test.c"),
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
            self.assertEqual(completed.stdout.strip(), "group engine tests passed")


if __name__ == "__main__":
    unittest.main()
