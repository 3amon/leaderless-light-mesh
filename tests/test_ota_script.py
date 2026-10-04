import os
import pathlib
import subprocess
import tempfile
import textwrap
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
OTA = ROOT / "pi-tools" / "ota.sh"


class OtaWrapperTests(unittest.TestCase):
    def run_wrapper(self, exit_code):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            fake = directory / "fake_deploy.py"
            arguments = directory / "arguments.txt"
            fake.write_text(
                textwrap.dedent(
                    f"""
                    import os
                    import pathlib
                    import sys
                    pathlib.Path(os.environ["ARGUMENTS"]).write_text("\\n".join(sys.argv[1:]))
                    raise SystemExit({exit_code})
                    """
                )
            )
            env = os.environ.copy()
            env.update({"DEPLOY": str(fake), "ARGUMENTS": str(arguments)})
            result = subprocess.run(
                ["bash", str(OTA), "opal-reef-tide", "image.signed.bin", "--upload-attempts", "2"],
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )
            return result, arguments.read_text().splitlines()

    def test_forwards_to_transactional_deploy_command(self):
        result, arguments = self.run_wrapper(0)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(
            arguments,
            ["deploy", "opal-reef-tide", "image.signed.bin", "--upload-attempts", "2"],
        )

    def test_preserves_nonzero_exit(self):
        result, _ = self.run_wrapper(3)
        self.assertEqual(result.returncode, 3)


if __name__ == "__main__":
    unittest.main()
