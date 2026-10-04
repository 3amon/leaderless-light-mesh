import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "pi-tools"))

from status_telemetry import evaluate_freshness, parse_status  # noqa: E402


class StatusParserTests(unittest.TestCase):
    def test_current_status_exposes_clock_freshness(self):
        parsed = parse_status(
            "opal up=42s state=healthy theta=-2ms ppm=3 nbr=1/1/1 age=481ms "
            "lapse=2 sched=scalable quiet=1799s mode=party/10 infra=1 usb=1 usbquiet=1 "
            "pat=4 boot=30/confirmed"
        )
        self.assertEqual(parsed["locked_peers"], 1)
        self.assertEqual(parsed["fresh_peers"], 1)
        self.assertEqual(parsed["tracked_peers"], 1)
        self.assertEqual(parsed["peer_age_ms"], 481)
        self.assertEqual(parsed["freshness_lapses"], 2)
        self.assertEqual(parsed["sync_profile"], "scalable")
        self.assertEqual(parsed["ota_quiet_s"], 1799)
        self.assertEqual(parsed["network_mode"], "party")
        self.assertEqual(parsed["mode_priority"], 10)
        self.assertEqual(parsed["usb_present"], 1)
        self.assertEqual(parsed["usb_quiet"], 1)
        self.assertEqual(parsed["boot_sequence"], 30)
        self.assertEqual(parsed["state"], "healthy")

    def test_old_status_remains_parseable(self):
        parsed = parse_status("feral up=9s theta=0ms ppm=-1 nbr=1/1 infra=1")
        self.assertEqual(parsed["locked_peers"], 1)
        self.assertEqual(parsed["fresh_peers"], 1)
        self.assertNotIn("tracked_peers", parsed)
        self.assertNotIn("freshness_lapses", parsed)


class FreshnessEvaluationTests(unittest.TestCase):
    def test_clean_run_passes_with_nonzero_starting_counter(self):
        records = [
            {
                "ring": "opal",
                "reply": "ok",
                "fresh_peers": 1,
                "peer_age_ms": age,
                "freshness_lapses": 7,
            }
            for age in (100, 600, 400)
        ]
        result = evaluate_freshness(records, ["opal"], expected_fresh=1)
        self.assertTrue(result["pass"])
        self.assertEqual(result["rings"]["opal"]["lapse_delta"], 0)
        self.assertEqual(result["rings"]["opal"]["max_peer_age_ms"], 600)

    def test_stale_missing_and_new_lapse_fail(self):
        records = [
            {
                "ring": "feral",
                "reply": "ok",
                "fresh_peers": 1,
                "freshness_lapses": 0,
            },
            {"ring": "feral", "reply": None},
            {
                "ring": "feral",
                "reply": "ok",
                "fresh_peers": 0,
                "freshness_lapses": 1,
            },
        ]
        result = evaluate_freshness(records, ["feral"], expected_fresh=1)
        ring = result["rings"]["feral"]
        self.assertFalse(result["pass"])
        self.assertEqual(ring["missing_samples"], 1)
        self.assertEqual(ring["stale_samples"], 1)
        self.assertEqual(ring["lapse_delta"], 1)

    def test_missing_counter_is_not_silently_accepted(self):
        records = [{"ring": "old", "reply": "ok", "fresh_peers": 1}]
        result = evaluate_freshness(records, ["old"], expected_fresh=1)
        self.assertFalse(result["pass"])
        self.assertIn("did not report", result["rings"]["old"]["reasons"][0])

    def test_reboot_during_soak_fails(self):
        records = [
            {
                "ring": "opal",
                "reply": "ok",
                "fresh_peers": 1,
                "freshness_lapses": 0,
                "boot_sequence": boot,
            }
            for boot in (30, 31)
        ]
        result = evaluate_freshness(records, ["opal"], expected_fresh=1)
        self.assertFalse(result["pass"])
        self.assertIn("boot sequence changed", result["rings"]["opal"]["reasons"][0])


if __name__ == "__main__":
    unittest.main()
