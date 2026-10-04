import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "pi-tools"))

import fade_test  # noqa: E402

S = fade_test.Sample
FPS = 30.0


def ramp(start_t, frames, a, b, colour):
    """Linear brightness ramp a->b over `frames`, in a fixed colour."""
    out = []
    for i in range(frames):
        k = a + (b - a) * (i / max(frames - 1, 1))
        out.append(S(start_t + i / FPS, colour[0] * k, colour[1] * k, colour[2] * k))
    return out


def hold(start_t, frames, level, colour):
    return [S(start_t + i / FPS, colour[0] * level, colour[1] * level,
              colour[2] * level) for i in range(frames)]


TEAL = (40, 200, 180)   # a plausible ring colour: nowhere near white or red


def good_restart():
    """lit -> 600ms fade out -> dark -> 800ms fade in -> lit."""
    seq = hold(0.0, 15, 1.0, TEAL)
    seq += ramp(seq[-1].t + 1 / FPS, 18, 1.0, 0.0, TEAL)      # 600 ms out
    seq += hold(seq[-1].t + 1 / FPS, 60, 0.0, TEAL)           # dark ~2 s
    seq += ramp(seq[-1].t + 1 / FPS, 24, 0.0, 1.0, TEAL)      # 800 ms in
    seq += hold(seq[-1].t + 1 / FPS, 15, 1.0, TEAL)
    return seq


class ClassifyTests(unittest.TestCase):
    def test_white_frame_is_caught(self):
        self.assertEqual(fade_test.classify_frame(S(0, 200, 200, 200), 220), "white")

    def test_red_frame_is_caught(self):
        self.assertEqual(fade_test.classify_frame(S(0, 200, 10, 10), 220), "red")

    def test_ring_colours_are_not_flagged(self):
        for colour in [(40, 200, 180), (30, 60, 220), (150, 40, 200), (20, 180, 90)]:
            self.assertIsNone(
                fade_test.classify_frame(S(0, *colour), 220), msg=str(colour))

    def test_dim_frames_are_not_flagged(self):
        # Low-level greyish frames during a fade are not a "white indicator".
        self.assertIsNone(fade_test.classify_frame(S(0, 10, 10, 10), 220))


class AnalyseTests(unittest.TestCase):
    def test_seamless_restart_passes(self):
        verdict = fade_test.analyse_fade(good_restart())
        self.assertEqual(verdict["failures"], [])
        self.assertTrue(verdict["passed"])

    def test_reported_spans_are_gradual_and_correctly_ordered(self):
        """Spans are threshold crossings, so they under-report the true ramp.

        Assert the shape rather than the constants: both transitions span real
        time, and the fade back in is the longer of the two because the
        firmware ramps up over 800 ms against 600 ms on the way out.
        """
        verdict = fade_test.analyse_fade(good_restart())
        self.assertGreater(verdict["fade_out_s"], 0.1)
        self.assertGreater(verdict["fade_in_s"], verdict["fade_out_s"])
        self.assertGreater(verdict["dark_s"], 1.0)

    def test_white_boot_marker_fails(self):
        """The exact regression: the early white marker punching through."""
        seq = good_restart()
        insert_at = next(i for i, s in enumerate(seq) if s.level == 0) + 10
        seq = seq[:insert_at] + hold(seq[insert_at].t, 3, 1.0, (230, 230, 230)) + \
            seq[insert_at:]
        verdict = fade_test.analyse_fade(seq)
        self.assertFalse(verdict["passed"])
        self.assertTrue(any("white" in f for f in verdict["failures"]))

    def test_red_fault_pulse_fails(self):
        seq = good_restart()
        insert_at = next(i for i, s in enumerate(seq) if s.level == 0) + 5
        seq = seq[:insert_at] + hold(seq[insert_at].t, 4, 1.0, (220, 8, 8)) + \
            seq[insert_at:]
        verdict = fade_test.analyse_fade(seq)
        self.assertFalse(verdict["passed"])
        self.assertTrue(any("red" in f for f in verdict["failures"]))

    def test_single_noisy_frame_is_not_an_indicator(self):
        """One dim reddish sample mid-fade is noise, not the fault pulse.

        A real hardware run flagged exactly this: rgb (67,18,16) at 32% of peak
        for a single frame. The genuine article lasts 800 ms at full brightness.
        """
        seq = good_restart()
        at = next(i for i, s in enumerate(seq) if s.level == 0) - 4
        seq = seq[:at] + [S(seq[at].t, 67, 18, 16)] + seq[at + 1:]
        verdict = fade_test.analyse_fade(seq)
        self.assertEqual(verdict["forbidden"], [])
        self.assertTrue(verdict["passed"])

    def test_sustained_red_pulse_is_still_caught(self):
        seq = good_restart()
        at = next(i for i, s in enumerate(seq) if s.level == 0)
        seq = seq[:at] + hold(seq[at].t, 6, 1.0, (220, 6, 6)) + seq[at:]
        verdict = fade_test.analyse_fade(seq)
        self.assertFalse(verdict["passed"])
        self.assertEqual(verdict["forbidden"][0]["kind"], "red")
        self.assertGreaterEqual(verdict["forbidden"][0]["frames"], 3)

    def test_hard_cut_is_not_a_fade(self):
        seq = hold(0.0, 15, 1.0, TEAL) + hold(0.5, 60, 0.0, TEAL) + \
            hold(2.5, 15, 1.0, TEAL)
        verdict = fade_test.analyse_fade(seq)
        self.assertFalse(verdict["passed"])
        self.assertTrue(any("cut rather than fade" in f for f in verdict["failures"]))

    def test_ring_that_never_returns_fails(self):
        seq = hold(0.0, 15, 1.0, TEAL) + ramp(0.5, 18, 1.0, 0.0, TEAL) + \
            hold(1.2, 60, 0.0, TEAL)
        verdict = fade_test.analyse_fade(seq)
        self.assertFalse(verdict["passed"])
        self.assertTrue(any("never came back" in f for f in verdict["failures"]))

    def test_no_restart_at_all_fails(self):
        verdict = fade_test.analyse_fade(hold(0.0, 90, 1.0, TEAL))
        self.assertFalse(verdict["passed"])
        self.assertTrue(any("never went dark" in f for f in verdict["failures"]))

    def test_dark_room_is_reported_not_silently_passed(self):
        verdict = fade_test.analyse_fade(hold(0.0, 90, 0.0, TEAL))
        self.assertFalse(verdict["passed"])
        self.assertTrue(any("no light" in f for f in verdict["failures"]))

    def test_too_few_samples_fails(self):
        verdict = fade_test.analyse_fade(hold(0.0, 3, 1.0, TEAL))
        self.assertFalse(verdict["passed"])
        self.assertTrue(any("too few samples" in f for f in verdict["failures"]))


if __name__ == "__main__":
    unittest.main()
