import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN_SOURCE = ROOT / "firmware" / "thread-ring" / "src" / "main.c"


class UsbQuietGroupProgressTests(unittest.TestCase):
    def test_group_control_plane_runs_before_usb_quiet_render_exit(self):
        source = MAIN_SOURCE.read_text()
        sleep_gate = source.index("if (render_usb_sleep_armed(now))")
        group_tick = source.index("groups_tick();", sleep_gate)
        quiet_gate = source.index("if (usb_quiet_display_active())", sleep_gate)
        quiet_render = source.index("render_usb_quiet(now);", quiet_gate)

        self.assertLess(group_tick, quiet_gate)
        self.assertLess(quiet_gate, quiet_render)


if __name__ == "__main__":
    unittest.main()
