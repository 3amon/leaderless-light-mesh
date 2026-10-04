import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "firmware" / "thread-ring" / "src"


class PeerFreshnessTests(unittest.TestCase):
    def test_host_transition_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "peer_freshness_test"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    f"-I{SOURCE_DIR}",
                    str(SOURCE_DIR / "peer_freshness.c"),
                    str(ROOT / "tests" / "c" / "peer_freshness_test.c"),
                    "-o",
                    str(executable),
                ],
                check=True,
                cwd=ROOT,
            )
            completed = subprocess.run(
                [str(executable)], check=True, cwd=ROOT, text=True, capture_output=True
            )
            self.assertEqual(completed.stdout.strip(), "peer freshness tests passed")


if __name__ == "__main__":
    unittest.main()
