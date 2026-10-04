import importlib.util
import json
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("hil_runner", ROOT / "pi-tools" / "hil_runner.py")
HIL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HIL)


def test_config():
    return {
        "schema": 1,
        "root": "/safe/test/root",
        "inventory": "config/rings.json",
        "retention": {
            "successful_runs_per_job": 1,
            "failed_runs_per_job": 2,
        },
        "jobs": {
            "pass": {
                "enabled": True,
                "risk": "observe",
                "schedules": ["nightly"],
                "timeout_seconds": 2,
                "argv": [
                    "{python}",
                    "-c",
                    "import pathlib,sys; pathlib.Path(sys.argv[1]).write_text('ok')",
                    "{run_dir}/evidence.txt",
                ],
            },
            "fail": {
                "enabled": False,
                "risk": "observe",
                "schedules": [],
                "timeout_seconds": 2,
                "argv": ["{python}", "-c", "raise SystemExit(7)"],
            },
            "reboot": {
                "enabled": False,
                "risk": "disruptive",
                "schedules": [],
                "timeout_seconds": 2,
                "argv": ["{python}", "-c", "pass"],
            },
        },
    }


class HilRunnerTests(unittest.TestCase):
    def setUp(self):
        self.devices = {
            "opal": {"address": "fd00::1", "usb_serial": "SERIAL1"},
        }

    def test_checked_config_and_inventory_expansion(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "jobs.json"
            path.write_text(json.dumps(test_config()))
            loaded = HIL.load_config(path)
            self.assertEqual(HIL.select_jobs(loaded, None, "nightly"), ["pass"])
            argv = HIL.expand_argv(
                ["{python}", "{repo}", "{run_dir}", "{ring:opal:address}"],
                ROOT,
                pathlib.Path("/tmp/run"),
                self.devices,
            )
            self.assertEqual(argv[-1], "fd00::1")
            with self.assertRaisesRegex(ValueError, "unknown ring inventory"):
                HIL.expand_argv(
                    ["{ring:missing:address}"], ROOT, pathlib.Path("/tmp/run"), self.devices
                )

            loaded["jobs"]["reboot"]["schedules"] = ["nightly"]
            path.write_text(json.dumps(loaded))
            with self.assertRaisesRegex(ValueError, "cannot be scheduled"):
                HIL.load_config(path)

    def test_checked_in_nightly_profile_is_observation_only_and_calibrated(self):
        config = HIL.load_config(ROOT / "config" / "hil-jobs.json")
        nightly = HIL.select_jobs(config, None, "nightly")
        self.assertEqual(nightly, ["boot-observe", "freshness", "rotation"])
        self.assertTrue(all(config["jobs"][name]["risk"] == "observe" for name in nightly))

        freshness_argv = config["jobs"]["freshness"]["argv"]
        threshold_index = freshness_argv.index("--max-missing-samples") + 1
        self.assertEqual(freshness_argv[threshold_index], "6")

        camera = config["jobs"]["camera-steady"]
        self.assertTrue(camera["enabled"])
        self.assertEqual(camera["schedules"], [])
        rings_index = camera["argv"].index("--rings") + 1
        self.assertEqual(
            camera["argv"][rings_index],
            "{ring:opal-reef-tide:address},{ring:feral-tide-light:address}",
        )
        expected_camera_limits = {
            "--min-pair-fraction": "0.9",
            "--max-abs-offset-ms": "10",
            "--min-offset-inlier-fraction": "0.65",
            "--max-jitter-ms": "5",
            "--max-abs-drift-ms-per-min": "2",
        }
        for flag, expected in expected_camera_limits.items():
            self.assertEqual(camera["argv"][camera["argv"].index(flag) + 1], expected)

        camera_unit = (ROOT / "deploy/systemd/thread-led-hil-camera.service").read_text()
        self.assertIn("ExecStartPre=+/usr/bin/systemctl stop fleet-dashboard.service", camera_unit)
        self.assertIn(
            "ExecStopPost=+/usr/bin/systemctl start --no-block fleet-dashboard.service",
            camera_unit,
        )
        self.assertIn("--job camera-steady", camera_unit)
        self.assertFalse((ROOT / "deploy/systemd/thread-led-hil-camera.timer").exists())

        rollback = config["jobs"]["rollback-canary"]
        self.assertFalse(rollback["enabled"])
        self.assertEqual(rollback["risk"], "disruptive")
        self.assertEqual(rollback["schedules"], [])
        self.assertEqual(rollback["inputs"], ["rollback_profile"])

        partition = config["jobs"]["partition-rejoin-canary"]
        self.assertFalse(partition["enabled"])
        self.assertEqual(partition["risk"], "disruptive")
        self.assertEqual(partition["schedules"], [])
        self.assertEqual(partition["inputs"], ["partition_profile"])

    def test_run_metadata_failure_timeout_and_disruptive_gate(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory) / "hil"
            root.mkdir()
            config = test_config()
            common = {
                "root": root,
                "repo": ROOT,
                "config_hash": "config-hash",
                "devices": self.devices,
                "inventory_hash": "inventory-hash",
                "allow_disruptive": False,
            }
            passed = HIL.run_job("pass", config["jobs"]["pass"], **common)
            self.assertEqual(passed["status"], "passed")
            self.assertTrue(pathlib.Path(passed["run_dir"], "evidence.txt").is_file())
            self.assertEqual(passed["config_sha256"], "config-hash")
            self.assertEqual(passed["inventory_sha256"], "inventory-hash")

            failed = HIL.run_job("fail", config["jobs"]["fail"], **common)
            self.assertEqual(failed["status"], "failed")
            self.assertEqual(failed["return_code"], 7)

            timeout_job = {
                "risk": "observe",
                "timeout_seconds": 0.05,
                "argv": ["{python}", "-c", "import time; time.sleep(30)"],
            }
            timed_out = HIL.run_job("timeout", timeout_job, **common)
            self.assertEqual(timed_out["status"], "timeout")
            self.assertTrue(timed_out["timed_out"])

            with self.assertRaisesRegex(PermissionError, "allow-disruptive"):
                HIL.run_job("reboot", config["jobs"]["reboot"], **common)
            self.assertEqual(len((root / "index.jsonl").read_text().splitlines()), 3)

    def test_retention_is_indexed_and_preserves_more_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory) / "hil"
            root.mkdir()
            config = test_config()
            common = {
                "root": root,
                "repo": ROOT,
                "config_hash": "config-hash",
                "devices": self.devices,
                "inventory_hash": "inventory-hash",
                "allow_disruptive": False,
            }
            for _ in range(3):
                HIL.run_job("pass", config["jobs"]["pass"], **common)
                HIL.run_job("fail", config["jobs"]["fail"], **common)
            removed = HIL.apply_retention(root, config)
            self.assertEqual(len(removed), 3)
            self.assertEqual(len(HIL.retained_runs(root / "runs" / "pass", "passed")), 1)
            self.assertEqual(len(HIL.retained_runs(root / "runs" / "fail", "failed")), 2)
            events = [json.loads(line) for line in (root / "index.jsonl").read_text().splitlines()]
            self.assertEqual(sum(event.get("event") == "retention_delete" for event in events), 3)

    def test_unsafe_roots_are_rejected(self):
        for path in (pathlib.Path("/"), pathlib.Path.home(), ROOT):
            with self.assertRaisesRegex(ValueError, "unsafe HIL root"):
                HIL.resolve_root(path, ROOT)

    def test_runtime_input_is_required_expanded_and_hashed(self):
        with tempfile.TemporaryDirectory() as directory:
            temporary = pathlib.Path(directory)
            input_path = temporary / "profile.json"
            input_path.write_text("{}\n")
            parsed = HIL.parse_inputs([f"profile={input_path}"])
            self.assertEqual(parsed["profile"], input_path.resolve())
            argv = HIL.expand_argv(
                ["{python}", "tool.py", "--profile", "{input:profile}"],
                ROOT,
                temporary / "run",
                self.devices,
                parsed,
            )
            self.assertEqual(argv[-1], str(input_path.resolve()))
            with self.assertRaisesRegex(ValueError, "not supplied"):
                HIL.expand_argv(["{input:profile}"], ROOT, temporary / "run", self.devices)
            with self.assertRaisesRegex(ValueError, "duplicate"):
                HIL.parse_inputs([f"profile={input_path}", f"profile={input_path}"])


if __name__ == "__main__":
    unittest.main()
