import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "rotation_test", ROOT / "pi-tools" / "rotation_test.py"
)
ROTATION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ROTATION)


def cycle(timestamp, opal, feral, opal_boot=4, feral_boot=7):
    def sample(name, pattern, boot):
        return {
            "reply": f"ring pat={pattern} boot={boot}/confirmed",
            "name": name,
            "pattern": pattern,
            "boot_sequence": boot,
        }

    return {
        "t": timestamp,
        "samples": {
            "opal": sample("opal", opal, opal_boot),
            "feral": sample("feral", feral, feral_boot),
        },
    }


class RotationEvaluationTests(unittest.TestCase):
    def test_default_covers_the_inventoried_five_ring_fleet(self):
        self.assertEqual(
            ROTATION.DEFAULT_RINGS,
            (
                "opal-reef-tide",
                "feral-tide-light",
                "lucid-iris-light",
                "glowing-bloom-wave",
                "dusty-fern-shimmer",
            ),
        )

    def evaluate(self, records, **overrides):
        thresholds = {
            "min_changes": 2,
            "min_interval": 55,
            "max_interval": 65,
            "max_mismatch_cycles": 1,
            "max_missing_samples": 1,
        }
        thresholds.update(overrides)
        return ROTATION.evaluate_rotation(records, ["opal", "feral"], **thresholds)

    def test_synchronized_minute_rotation_passes(self):
        result = self.evaluate(
            [cycle(0, 1, 1), cycle(10, 1, 1), cycle(30, 2, 1), cycle(31, 2, 2), cycle(91, 3, 3)]
        )
        self.assertTrue(result["pass"], result)
        self.assertEqual(result["pattern_changes"], 2)
        self.assertEqual(result["intervals_seconds"], [60])
        self.assertEqual(result["mismatch_cycles"], 1)

    def test_missing_mismatch_bad_cadence_and_reboot_fail(self):
        records = [
            cycle(0, 1, 1),
            {"t": 20, "samples": {"opal": {"reply": None}, "feral": {"reply": None}}},
            cycle(30, 2, 1),
            cycle(31, 2, 1),
            cycle(40, 2, 2, opal_boot=5),
            cycle(80, 3, 3, opal_boot=5),
        ]
        result = self.evaluate(records, max_mismatch_cycles=0)
        self.assertFalse(result["pass"])
        self.assertTrue(any("missing samples" in reason for reason in result["reasons"]))
        self.assertTrue(any("mismatch cycles" in reason for reason in result["reasons"]))
        self.assertTrue(any("boot sequence changed" in reason for reason in result["reasons"]))
        self.assertTrue(any("intervals outside" in reason for reason in result["reasons"]))


if __name__ == "__main__":
    unittest.main()
