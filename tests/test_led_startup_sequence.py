import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/thread-ring/src/main.c"
MCUBOOT_OVERLAY = ROOT / "firmware/thread-ring/sysbuild/mcuboot.overlay"


class LedStartupSequenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = MAIN.read_text()
        cls.main = cls.source[cls.source.index("int main(void)") :]

    def test_startup_keeps_led_rail_off_through_initialization(self):
        ordered = (
            "int power_rc = led_power_prepare();",
            "int layout_rc = led_config_init();",
            "int usb_rc = usb_enable(NULL);",
            "hb_init();",
            "power_rc = led_power_enable();",
            "show_led_startup_glow(wdt, wdt_ch);",
            "while (1)",
        )
        positions = [self.main.index(marker) for marker in ordered]
        self.assertEqual(positions, sorted(positions))

    def test_startup_delay_settle_and_dim_glow_are_bounded(self):
        self.assertIn("#define LED_POWER_STARTUP_DELAY_MS 2000", self.source)
        self.assertIn("#define LED_POWER_SETTLE_MS 100", self.source)
        self.assertIn("#define LED_STARTUP_GLOW_MS 3000", self.source)
        self.assertIn("#define LED_STARTUP_GLOW_LEVEL 8", self.source)

    def test_first_frame_after_power_enable_is_black(self):
        start = self.source.index("static int led_power_enable(void)")
        end = self.source.index("static int led_power_start(void)", start)
        enable = self.source[start:end]
        ordered = (
            "gpio_pin_set_dt(selected, 1)",
            "k_sleep(K_MSEC(LED_POWER_SETTLE_MS))",
            "memset(pixels, 0, sizeof(pixels))",
            "led_strip_update_rgb(strip, pixels, LED_LAYOUT_MAX_PIXELS)",
        )
        positions = [enable.index(marker) for marker in ordered]
        self.assertEqual(positions, sorted(positions))

    def test_ota_holds_led_power_off_and_has_no_progress_render(self):
        self.assertIn("#define DFU_POWER_HOLD_MS 30000", self.source)
        self.assertIn("led_power_stop();\n\t\t\tdfu_power_held = true;", self.source)
        self.assertIn("Intentionally dark", self.source)
        self.assertNotIn("OTA progress arc", self.source)

    def test_every_controlled_reboot_quiesces_led_power_and_data(self):
        start = self.source.index("static void clean_reboot")
        end = self.source.index("/* This node's deterministic", start)
        reboot = self.source[start:end]
        self.assertLess(
            reboot.index("led_power_quiesce_for_reset()"),
            reboot.index("sys_reboot(SYS_REBOOT_COLD)"),
        )

    def test_mcuboot_holds_both_led_rails_low(self):
        overlay = MCUBOOT_OVERLAY.read_text()
        self.assertIn("gpios = <17 GPIO_ACTIVE_HIGH>;", overlay)
        self.assertIn("gpios = <13 GPIO_ACTIVE_HIGH>;", overlay)
        self.assertEqual(overlay.count("output-low;"), 2)


if __name__ == "__main__":
    unittest.main()
