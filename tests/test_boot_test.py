import argparse
import importlib.util
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
PI_TOOLS = ROOT / "pi-tools"
sys.path.insert(0, str(PI_TOOLS))
SCRIPT = PI_TOOLS / "boot_test.py"
SPEC = importlib.util.spec_from_file_location("boot_test", SCRIPT)
BOOT = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(BOOT)


class BootTestParsingTests(unittest.TestCase):
    def test_parses_status_fields(self):
        status = BOOT.parse_status(
            "opal-reef-tide up=3817s theta=42ms infra=1 "
            "part=5991ee41 reset=00000012/none pat=5 bright=45%"
        )
        self.assertEqual(status["name"], "opal-reef-tide")
        self.assertEqual(status["up"], 3817)
        self.assertEqual(status["infra"], 1)
        self.assertEqual(status["part"], "5991ee41")
        self.assertEqual(status["reset"], "00000012/none")

    def test_parses_plain_integer(self):
        status = BOOT.parse_status("opal-reef-tide up=4 infra=1")
        self.assertEqual(status["up"], 4)
        self.assertEqual(status["infra"], 1)

    def test_empty_status_is_none(self):
        self.assertIsNone(BOOT.parse_status(None))
        self.assertIsNone(BOOT.parse_status(""))

    def test_parses_persistent_boot_trail(self):
        parsed = BOOT.parse_boot_status(
            "boot=31 stage=confirmed mcuboot=d6/a7 "
            "trail=30:confirmed@2341,31:entry@0,"
            "31:usb-quiesced@512,31:confirmed@2298"
        )
        self.assertEqual(parsed["boot"], 31)
        self.assertEqual(parsed["stage"], "confirmed")
        self.assertEqual(parsed["mcuboot_phase"], 0xD6)
        self.assertEqual(parsed["mcuboot_detail"], 0xA7)
        self.assertEqual(
            parsed["events"],
            [
                {"boot": 30, "stage": "confirmed", "uptime_ms": 2341},
                {"boot": 31, "stage": "entry", "uptime_ms": 0},
                {"boot": 31, "stage": "usb-quiesced", "uptime_ms": 512},
                {"boot": 31, "stage": "confirmed", "uptime_ms": 2298},
            ],
        )

    def test_parses_legacy_boot_trail_without_mcuboot_trace(self):
        parsed = BOOT.parse_boot_status("boot=7 stage=confirmed trail=7:entry@0,7:confirmed@1234")
        self.assertEqual(parsed["boot"], 7)
        self.assertNotIn("mcuboot_phase", parsed)

    def test_unrecognized_boot_reply_keeps_raw_evidence(self):
        parsed = BOOT.parse_boot_status("boot status overflow")
        self.assertEqual(parsed["raw"], "boot status overflow")
        self.assertIn("parse_error", parsed)

    def test_restart_detected_by_uptime_rollback(self):
        self.assertTrue(BOOT.status_is_restarted({"up": 3}, 20, False, True))

    def test_restart_detected_by_observed_full_outage(self):
        self.assertTrue(BOOT.status_is_restarted({"up": 17}, 11, True, True))

    def test_partial_outage_is_not_enough(self):
        self.assertFalse(BOOT.status_is_restarted({"up": 17}, 11, False, True))

    def test_network_only_outage_is_enough(self):
        self.assertTrue(BOOT.status_is_restarted({"up": 17}, 11, False, True, usb_required=False))

    def test_absent_usb_serial_has_no_path(self):
        self.assertIsNone(BOOT.usb_path(None))

    def test_failure_domain_distinguishes_infrastructure(self):
        self.assertEqual(BOOT.failure_domain(False, True), "infrastructure")
        self.assertEqual(BOOT.failure_domain(False, False), "target")
        self.assertIsNone(BOOT.failure_domain(True, True))

    def test_evidence_arguments_serialize_optional_paths(self):
        args = argparse.Namespace(
            target="feral-tide-light",
            control_key_dir=pathlib.Path("/etc/thread-led-node/control-keys"),
            trials=3,
        )
        self.assertEqual(
            BOOT.json_safe_args(args),
            {
                "target": "feral-tide-light",
                "control_key_dir": "/etc/thread-led-node/control-keys",
                "trials": 3,
            },
        )

    def test_strict_observation_requires_identity_image_infra_and_boot(self):
        BOOT.validate_target_health(
            "opal-reef-tide",
            "fd00::1",
            "0.1.22",
            "FD00::1",
            {"img": "0.1.22"},
            {"name": "opal-reef-tide", "infra": 1},
            {"stage": "confirmed", "events": []},
        )
        bad_boot = {"raw": "unparsed", "parse_error": "bad", "events": []}
        with self.assertRaisesRegex(RuntimeError, "boot trail"):
            BOOT.validate_target_health(
                "opal-reef-tide",
                "fd00::1",
                "0.1.22",
                "fd00::1",
                {"img": "0.1.22"},
                {"name": "opal-reef-tide", "infra": 1},
                bad_boot,
            )


if __name__ == "__main__":
    unittest.main()
