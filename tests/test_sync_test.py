import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
PI_TOOLS = ROOT / "pi-tools"
sys.path.insert(0, str(PI_TOOLS))
SPEC = importlib.util.spec_from_file_location("sync_harness", PI_TOOLS / "sync_test.py")
SYNC = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = SYNC
SPEC.loader.exec_module(SYNC)


class CameraHarnessTests(unittest.TestCase):
    @staticmethod
    def child(source):
        return subprocess.Popen(
            [sys.executable, "-u", "-c", source],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

    def test_discovery_deadline_is_not_blocked_by_quiet_child(self):
        with tempfile.TemporaryDirectory() as directory:
            run = SYNC.Run(directory)
            child = self.child("import time; time.sleep(60)")
            started = time.monotonic()
            with mock.patch.object(SYNC.subprocess, "Popen", return_value=child):
                with self.assertRaisesRegex(RuntimeError, "discovery never finished"):
                    SYNC.start_camera(run, 1, 30, discovery_timeout=0.05)
            self.assertLess(time.monotonic() - started, 2)
            self.assertIsNotNone(child.poll())
            run.close()

    def test_ready_camera_is_stopped_and_output_is_logged(self):
        source = (
            "import time; "
            "print('# blob 0: ready', flush=True); "
            "print('# measuring endlessly at target 30 fps', flush=True); "
            "time.sleep(60)"
        )
        with tempfile.TemporaryDirectory() as directory:
            run = SYNC.Run(directory)
            child = self.child(source)
            with mock.patch.object(SYNC.subprocess, "Popen", return_value=child):
                camera = SYNC.start_camera(run, 1, 30, discovery_timeout=1)
            SYNC.stop_camera(camera)
            self.assertIsNotNone(child.poll())
            self.assertIn("# measuring", (pathlib.Path(directory) / "log.txt").read_text())
            run.close()

    def test_blob_count_mismatch_is_a_hard_failure(self):
        source = (
            "import time; "
            "print('# blob 0: ready', flush=True); "
            "print('# measuring endlessly at target 30 fps', flush=True); "
            "time.sleep(60)"
        )
        with tempfile.TemporaryDirectory() as directory:
            run = SYNC.Run(directory)
            child = self.child(source)
            with mock.patch.object(SYNC.subprocess, "Popen", return_value=child):
                with self.assertRaisesRegex(RuntimeError, "found 1 blobs, expected 2"):
                    SYNC.start_camera(run, 2, 30, discovery_timeout=1)
            self.assertIsNotNone(child.poll())
            run.close()

    def test_setup_failure_restores_every_ring(self):
        replies = []

        def fake_cmd(addr, command, timeout=5, **kwargs):
            replies.append((addr, command))
            return "ok"

        with tempfile.TemporaryDirectory() as directory:
            argv = [
                "sync_test.py",
                "steady",
                "--rings",
                "first,second",
                "--outdir",
                directory,
            ]
            with (
                mock.patch.object(sys, "argv", argv),
                mock.patch.object(SYNC.ringctl, "resolve", side_effect=["fd00::1", "fd00::2"]),
                mock.patch.object(SYNC, "cmd", side_effect=fake_cmd),
                mock.patch.object(
                    SYNC, "start_camera", side_effect=RuntimeError("camera unavailable")
                ),
                self.assertRaisesRegex(RuntimeError, "camera unavailable"),
            ):
                SYNC.main()

        for address in ("fd00::1", "fd00::2"):
            self.assertIn((address, "skew off"), replies)
            self.assertIn((address, "flash off"), replies)

    def test_steady_qualification_has_explicit_timing_thresholds(self):
        passing = {
            "pairs": [
                {
                    "blob": 1,
                    "n": 301,
                    "offset_s": 0.0036,
                    "_offset_samples_s": [0.0036] * 301,
                    "jitter_s": 0.0032,
                    "drift_s_per_min": -0.00093,
                }
            ]
        }
        result = SYNC.qualify_steady(passing, 300, 1000, 0.9, 10, 0.65, 5, 2)
        self.assertTrue(result["pass"])
        self.assertEqual(result["thresholds"]["minimum_pairs"], 270)

        for field, value, reason in (
            ("n", 269, "paired flashes"),
            ("offset_s", 0.011, "absolute offset"),
            ("jitter_s", 0.006, "jitter"),
            ("drift_s_per_min", 0.003, "absolute drift"),
        ):
            pair = dict(passing["pairs"][0], _offset_samples_s=[0.0036] * 301, **{field: value})
            failing = {"pairs": [pair]}
            result = SYNC.qualify_steady(failing, 300, 1000, 0.9, 10, 0.65, 5, 2)
            self.assertFalse(result["pass"])
            self.assertTrue(any(reason in item for item in result["pairs"][0]["reasons"]))

        failing = {
            "pairs": [
                dict(
                    passing["pairs"][0],
                    _offset_samples_s=[0.003] * 190 + [0.02] * 111,
                )
            ]
        }
        result = SYNC.qualify_steady(failing, 300, 1000, 0.9, 10, 0.65, 5, 2)
        self.assertFalse(result["pass"])
        self.assertTrue(any("inlier fraction" in item for item in result["pairs"][0]["reasons"]))

        result = SYNC.qualify_steady({"pairs": []}, 300, 1000, 0.9, 10, 0.65, 5, 2)
        self.assertFalse(result["pass"])
        self.assertIn("no ring pairs", result["reasons"][0])


if __name__ == "__main__":
    unittest.main()
