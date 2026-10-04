import os
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts"))
import signing


class SigningTests(unittest.TestCase):
    def private_key(self, mode=0o600):
        descriptor, name = tempfile.mkstemp()
        os.close(descriptor)
        path = pathlib.Path(name)
        self.addCleanup(path.unlink, missing_ok=True)
        path.chmod(mode)
        return path

    def test_private_key_rejects_group_or_other_access(self):
        with self.assertRaisesRegex(ValueError, "group/other"):
            signing.private_file(self.private_key(0o640), "test key")

    @mock.patch("signing.subprocess.run")
    def test_firmware_key_rejects_mcuboot_development_identity(self, run):
        run.return_value.stdout = bytes.fromhex(
            "fc5701dc6135e1323847bdc40f04d2e5bee5833b23c29f93593d00018cfa9994"
        )
        with self.assertRaisesRegex(ValueError, "development signing key"):
            signing.firmware_key_info(pathlib.Path("/workspace"), self.private_key())

    @mock.patch("signing.subprocess.run")
    def test_firmware_key_reports_project_public_hash(self, run):
        expected = bytes(range(32))
        run.return_value.stdout = expected
        result = signing.firmware_key_info(pathlib.Path("/workspace"), self.private_key())
        self.assertEqual(result["public_key_sha256"], expected.hex())


if __name__ == "__main__":
    unittest.main()
