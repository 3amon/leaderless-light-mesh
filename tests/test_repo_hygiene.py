import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "check_repo_hygiene", ROOT / "scripts" / "check_repo_hygiene.py"
)
repo_hygiene = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(repo_hygiene)


class RepositoryHygieneTests(unittest.TestCase):
    def test_rejects_generated_paths(self):
        generated = {
            "firmware/thread-ring/build-ota/zephyr/zephyr.elf",
            "firmware/ring-test/build/zephyr/zephyr.bin",
            "pi-tools/__pycache__/ringctl.cpython-312.pyc",
            ".cache/zephyr-ws/zephyr/.git/HEAD",
            "ci-build/firmware/size-report.txt",
            "dist/v0.1.17/zephyr.signed.bin",
        }
        for value in generated:
            with self.subTest(value=value):
                self.assertIsNotNone(repo_hygiene.generated_reason(pathlib.PurePosixPath(value)))

    def test_accepts_source_and_evidence(self):
        source = {
            "firmware/thread-ring/src/main.c",
            "pi-tools/ringctl.py",
            "evidence/hardware/2026-07-20/manifest.json",
            "ci/Dockerfile",
        }
        for value in source:
            with self.subTest(value=value):
                self.assertIsNone(repo_hygiene.generated_reason(pathlib.PurePosixPath(value)))


if __name__ == "__main__":
    unittest.main()
