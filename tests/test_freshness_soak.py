import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "freshness_soak", ROOT / "pi-tools" / "freshness_soak.py"
)
FRESHNESS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FRESHNESS)


class FreshnessFleetDefaultsTests(unittest.TestCase):
    def test_default_covers_the_inventoried_five_ring_fleet(self):
        self.assertEqual(
            FRESHNESS.DEFAULT_RINGS,
            (
                "opal-reef-tide",
                "feral-tide-light",
                "lucid-iris-light",
                "glowing-bloom-wave",
                "dusty-fern-shimmer",
            ),
        )


if __name__ == "__main__":
    unittest.main()
