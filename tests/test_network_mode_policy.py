import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "firmware" / "thread-ring" / "src"


class NetworkModePolicyTests(unittest.TestCase):
    def test_host_policy_and_wire_contract(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "network_mode_policy_test"
            subprocess.run(
                [
                    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    f"-I{SOURCE_DIR}",
                    str(SOURCE_DIR / "network_mode_policy.c"),
                    str(ROOT / "tests" / "c" / "network_mode_policy_test.c"),
                    "-o", str(executable),
                ],
                check=True,
                cwd=ROOT,
            )
            completed = subprocess.run(
                [str(executable)], check=True, cwd=ROOT, text=True, capture_output=True
            )
            self.assertEqual(completed.stdout.strip(), "network mode policy tests passed")


if __name__ == "__main__":
    unittest.main()
