import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts"))
import version_info


class VersionInfoTests(unittest.TestCase):
    def repo(self):
        temporary = tempfile.TemporaryDirectory()
        root = pathlib.Path(temporary.name)
        subprocess.run(["git", "init", "-q", str(root)], check=True)
        subprocess.run(["git", "-C", str(root), "config", "user.name", "Test"], check=True)
        subprocess.run(
            ["git", "-C", str(root), "config", "user.email", "test@example.com"], check=True
        )
        (root / "file").write_text("one\n")
        subprocess.run(["git", "-C", str(root), "add", "file"], check=True)
        subprocess.run(["git", "-C", str(root), "commit", "-qm", "first"], check=True)
        return temporary, root

    def test_exact_release_tag_sets_image_version(self):
        temporary, root = self.repo()
        self.addCleanup(temporary.cleanup)
        subprocess.run(["git", "-C", str(root), "tag", "v1.2.3"], check=True)
        info = version_info.derive(root)
        self.assertEqual(info["version"], "1.2.3")
        self.assertEqual(info["exact_tag"], "v1.2.3")
        self.assertFalse(info["dirty"])

    def test_untagged_build_has_numeric_metadata(self):
        temporary, root = self.repo()
        self.addCleanup(temporary.cleanup)
        info = version_info.derive(root)
        self.assertRegex(info["version"], r"^0\.0\.0\+\d+$")

    def test_dirty_state_is_reported(self):
        temporary, root = self.repo()
        self.addCleanup(temporary.cleanup)
        (root / "file").write_text("changed\n")
        self.assertTrue(version_info.derive(root)["dirty"])

    def test_override_is_strict_numeric_mcuboot_version(self):
        temporary, root = self.repo()
        self.addCleanup(temporary.cleanup)
        self.assertEqual(version_info.derive(root, "2.4.6+8")["version"], "2.4.6+8")
        with self.assertRaises(ValueError):
            version_info.derive(root, "2.4.6-rc1")
