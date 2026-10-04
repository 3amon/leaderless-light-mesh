import importlib.util
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).parents[1] / "pi-tools" / "group_mobility_test.py"
SPEC = importlib.util.spec_from_file_location("group_mobility_test", MODULE_PATH)
MOBILITY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MOBILITY)


def sample(
    sid,
    target,
    agreed,
    lit,
    size,
    tier,
    *,
    complete=1,
    committed=None,
    handoff_ticks=0,
    visibility=None,
    fade=1.0,
):
    return {
        "sid": sid,
        "target": target,
        "agreed": agreed,
        "committed": agreed if committed is None and lit else (committed or 0),
        "handoff_ticks": handoff_ticks,
        "lit": lit,
        "visibility": float(lit if visibility is None else visibility),
        "fade": fade,
        "size": size,
        "tier": tier,
        "topology_complete": complete,
    }


class GroupMobilityTests(unittest.TestCase):
    def test_group_parser_requires_new_topology_fields(self):
        parsed = MOBILITY.parse_group(
            "sid=13 view=00802200 target=00802200 topo=1 "
            "agreed=00802200 commit=00802200 hand=2 "
            "lit=1 vis=1.00 fade=1.00 size=3 tier=crowd"
        )
        self.assertEqual(parsed["sid"], 13)
        self.assertEqual(parsed["target"], 0x00802200)
        self.assertEqual(parsed["committed"], 0x00802200)
        self.assertEqual(parsed["handoff_ticks"], 2)
        self.assertEqual(parsed["visibility"], 1.0)
        self.assertFalse(parsed.get("parse_error", False))
        self.assertTrue(MOBILITY.parse_group("sid=13 old-format")["parse_error"])
        self.assertIsNone(MOBILITY.parse_group(None)["reply"])

    def test_crowd_split_and_rejoin_snapshots(self):
        names = ["opal", "feral", "lucid"]
        all_bits = (1 << 13) | (1 << 9) | (1 << 23)
        pair_bits = (1 << 9) | (1 << 23)
        crowd = {
            "opal": sample(13, all_bits, all_bits, 1, 3, "crowd"),
            "feral": sample(9, all_bits, all_bits, 1, 3, "crowd"),
            "lucid": sample(23, all_bits, all_bits, 1, 3, "crowd"),
        }
        self.assertTrue(MOBILITY.phase_matches(crowd, names, "opal", "crowd"))

        split = {
            "opal": sample(13, 1 << 13, 1 << 13, 0, 1, "solo"),
            "feral": sample(9, pair_bits, pair_bits, 1, 2, "duo"),
            "lucid": sample(23, pair_bits, pair_bits, 1, 2, "duo"),
        }
        self.assertTrue(MOBILITY.phase_matches(split, names, "opal", "split"))
        split["feral"]["agreed"] = all_bits
        self.assertFalse(MOBILITY.phase_matches(split, names, "opal", "split"))

    def test_four_ring_split_uses_crowd_resident_tier(self):
        names = ["a", "b", "c", "d"]
        all_bits = sum(1 << sid for sid in range(1, 5))
        resident_bits = all_bits & ~(1 << 1)
        samples = {
            "a": sample(1, 1 << 1, 1 << 1, 0, 1, "solo"),
            "b": sample(2, resident_bits, resident_bits, 1, 3, "crowd"),
            "c": sample(3, resident_bits, resident_bits, 1, 3, "crowd"),
            "d": sample(4, resident_bits, resident_bits, 1, 3, "crowd"),
        }
        self.assertTrue(MOBILITY.phase_matches(samples, names, "a", "split"))

    def test_residents_hold_group_through_split_and_rejoin(self):
        names = ["a", "b", "c"]
        all_bits = sum(1 << sid for sid in range(1, 4))
        resident_bits = all_bits & ~(1 << 1)
        records = [
            {
                "t": 1.0,
                "phase": "split",
                "samples": {
                    "a": sample(1, 1 << 1, all_bits, 1, 3, "crowd", handoff_ticks=2),
                    "b": sample(
                        2,
                        resident_bits,
                        all_bits,
                        1,
                        3,
                        "crowd",
                        committed=all_bits,
                        handoff_ticks=2,
                    ),
                    "c": sample(
                        3,
                        resident_bits,
                        all_bits,
                        1,
                        3,
                        "crowd",
                        committed=all_bits,
                        handoff_ticks=2,
                    ),
                },
            },
            {
                "t": 5.0,
                "phase": "rejoined",
                "samples": {
                    "a": sample(1, all_bits, all_bits, 1, 3, "crowd"),
                    "b": sample(2, all_bits, resident_bits, 1, 2, "duo", committed=resident_bits),
                    "c": sample(3, all_bits, resident_bits, 1, 2, "duo", committed=resident_bits),
                },
            },
        ]
        result = MOBILITY.resident_continuity(records, names, "a", 0)
        self.assertTrue(result["pass"], result)
        self.assertEqual(result["max_handoff_ticks"], {"b": 2, "c": 2})

        records[1]["samples"]["b"] = sample(2, all_bits, 1 << 2, 0, 1, "solo")
        result = MOBILITY.resident_continuity(records, names, "a", 0)
        self.assertFalse(result["pass"])
        self.assertTrue(result["violations"])

    def test_incomplete_or_overlapping_state_never_passes(self):
        names = ["a", "b", "c"]
        all_bits = (1 << 1) | (1 << 2) | (1 << 3)
        samples = {
            "a": sample(1, all_bits, all_bits, 1, 3, "crowd"),
            "b": sample(2, all_bits, all_bits, 1, 3, "crowd"),
            "c": sample(3, all_bits, all_bits, 1, 3, "crowd", complete=0),
        }
        self.assertFalse(MOBILITY.phase_matches(samples, names, "a", "crowd"))


if __name__ == "__main__":
    unittest.main()
