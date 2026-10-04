import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "firmware" / "thread-ring" / "src"


class PeerSlotPolicyTests(unittest.TestCase):
    def test_host_policy_traces(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "peer_slot_policy_test"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    f"-I{SOURCE_DIR}",
                    str(SOURCE_DIR / "peer_slot_policy.c"),
                    str(ROOT / "tests" / "c" / "peer_slot_policy_test.c"),
                    "-o",
                    str(executable),
                ],
                check=True,
                cwd=ROOT,
            )
            completed = subprocess.run(
                [str(executable)], check=True, cwd=ROOT, text=True, capture_output=True
            )
            self.assertEqual(completed.stdout.strip(), "peer slot policy tests passed")


if __name__ == "__main__":
    unittest.main()
