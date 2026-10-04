import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def config_values(path: pathlib.Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw_line in path.read_text().splitlines():
        line = raw_line.strip()
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
    return values


class UsbPowerConfigTests(unittest.TestCase):
    def test_usb_profiles_are_bus_powered_and_request_500ma(self):
        profiles = (
            ROOT / "firmware/thread-ring/prj.conf",
            ROOT / "firmware/ring-test/prj.conf",
        )
        for profile in profiles:
            with self.subTest(profile=profile.relative_to(ROOT)):
                config = config_values(profile)
                self.assertEqual(config.get("CONFIG_USB_SELF_POWERED"), "n")
                # USB_MAX_POWER is encoded in 2 mA units; 250 requests 500 mA.
                self.assertEqual(config.get("CONFIG_USB_MAX_POWER"), "250")


if __name__ == "__main__":
    unittest.main()
