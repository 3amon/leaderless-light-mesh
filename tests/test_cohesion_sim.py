import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "firmware" / "thread-ring" / "src"


class CohesionSimTests(unittest.TestCase):
    def test_adversarial_scenarios(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "cohesion_sim_test"
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
                    str(SOURCE_DIR / "peer_slot_policy.c"),
                    str(SOURCE_DIR / "sync_schedule.c"),
                    str(ROOT / "tests" / "c" / "cohesion_sim_test.c"),
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
            self.assertEqual(completed.stdout.strip(), "cohesion simulator scenarios passed")


if __name__ == "__main__":
    unittest.main()
