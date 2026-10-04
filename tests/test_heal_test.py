import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "pi-tools"))

import heal_test  # noqa: E402


def status_line(boot, reset="00000002/heal", state="healthy", up=5):
    return f"opal up={up}s state={state} reset={reset} boot={boot}/confirmed build=v0.1.50"


class StatusParsingTests(unittest.TestCase):
    def test_parses_fields_from_a_status_reply(self):
        with mock.patch.object(heal_test.socket, "socket") as sock:
            sock.return_value.recvfrom.return_value = (status_line(413).encode(), None)
            fields = heal_test.status("fd00::1")
        self.assertEqual(fields["boot"], "413/confirmed")
        self.assertEqual(fields["reset"], "00000002/heal")
        self.assertEqual(fields["state"], "healthy")

    def test_unreachable_device_yields_empty_not_an_exception(self):
        with mock.patch.object(heal_test.socket, "socket") as sock:
            sock.return_value.sendto.side_effect = OSError("no route")
            self.assertEqual(heal_test.status("fd00::1"), {})


class RebootWaitTests(unittest.TestCase):
    def test_tolerates_unreachable_while_rebooting(self):
        # A rebooting ring stops answering; that is expected mid-flight and
        # must not be mistaken for failure until the deadline.
        replies = [{}, {}, {"boot": "414/confirmed"}]

        with mock.patch.object(heal_test, "status", side_effect=replies), \
             mock.patch.object(heal_test.time, "sleep"):
            result = heal_test.wait_for_reboot("fd00::1", 413, 60)
        self.assertEqual(result["boot"], "414/confirmed")

    def test_gives_up_at_the_deadline(self):
        with mock.patch.object(heal_test, "status", return_value={"boot": "413/confirmed"}), \
             mock.patch.object(heal_test.time, "sleep"), \
             mock.patch.object(heal_test.time, "monotonic", side_effect=[0, 0, 61]):
            self.assertEqual(heal_test.wait_for_reboot("fd00::1", 413, 60), {})


class RetryTests(unittest.TestCase):
    """A dropped reply right after a reboot must not fail the run."""

    def test_retries_a_transient_transport_error(self):
        calls = []

        def flaky(address, device, text, timeout):
            calls.append(text)
            if len(calls) < 3:
                raise RuntimeError("device did not return an authenticated response")
            return "heal interval=60000"

        with mock.patch.object(heal_test.control_client,
                               "authenticated_udp_command", flaky), \
             mock.patch.object(heal_test.time, "sleep"):
            self.assertEqual(
                heal_test.command("fd00::1", "opal", "heal fire", 1.0),
                "heal interval=60000")
        self.assertEqual(len(calls), 3)

    def test_gives_up_and_reraises_after_the_last_attempt(self):
        with mock.patch.object(heal_test.control_client, "authenticated_udp_command",
                               side_effect=RuntimeError("gone")), \
             mock.patch.object(heal_test.time, "sleep"):
            with self.assertRaises(RuntimeError):
                heal_test.command("fd00::1", "opal", "heal fire", 1.0, attempts=2)


class SettleTests(unittest.TestCase):
    def test_waits_for_healthy_before_returning(self):
        replies = [{}, {"state": "booting"}, {"state": "healthy", "boot": "421/confirmed"}]
        with mock.patch.object(heal_test, "status", side_effect=replies), \
             mock.patch.object(heal_test.time, "sleep"):
            self.assertEqual(heal_test.wait_until_settled("fd00::1")["state"], "healthy")


class EscalationRunTests(unittest.TestCase):
    """The behaviour that only hardware could previously prove."""

    @staticmethod
    def _health(boot):
        return {"boot": f"{boot}/confirmed", "reset": "00000002/heal",
                "state": "healthy", "up": "3"}

    def _run(self, reported_intervals, boots_after, steps=2):
        """Simulate `steps` steps.

        reported_intervals -- what the device claims per step (the assertion)
        boots_after        -- the boot ordinal observed after each step's reboot
        """
        start_boot = 413
        with tempfile.TemporaryDirectory() as d:
            inventory = pathlib.Path(d) / "rings.json"
            inventory.write_text(json.dumps(
                {"schema": 1, "devices": [{"name": "opal", "address": "fd00::1"}]}))
            args = heal_test.argparse.Namespace(
                target="opal", expected_address=None, inventory=inventory,
                steps=steps, timeout=1.0, reboot_timeout=5.0,
                out=pathlib.Path(d) / "out.json",
            )

            # before each step: previous boot ordinal; baseline + per-step
            # settled check + final all come from status().
            before = [start_boot] + boots_after[:-1]
            replies = []
            for interval in reported_intervals:
                replies += [f"heal alone=0 fire=0 attempts=0 interval={interval}",
                            "ok", "ok"]
            replies += ["ok", "ok"]   # restore: clear, attempts 0

            with mock.patch.object(heal_test, "status",
                                   side_effect=[self._health(start_boot)] +
                                   [self._health(b) for b in boots_after] +
                                   [self._health(boots_after[-1])] * 3), \
                 mock.patch.object(heal_test, "wait_until_settled",
                                   side_effect=[self._health(b) for b in before]), \
                 mock.patch.object(heal_test, "command", side_effect=replies), \
                 mock.patch.object(heal_test, "wait_for_reboot",
                                   side_effect=[self._health(b) for b in boots_after]), \
                 mock.patch.object(heal_test.time, "sleep"):
                return heal_test.run(args)

    def test_passes_when_each_step_reboots_once_with_the_right_interval(self):
        result = self._run(reported_intervals=[60000, 180000],
                           boots_after=[414, 415])
        self.assertEqual(result["failures"], [])
        self.assertTrue(result["passed"])

    def test_fails_when_the_device_reports_the_wrong_interval(self):
        # The exact regression: a device stuck on the first step forever.
        result = self._run(reported_intervals=[60000, 60000],
                           boots_after=[414, 415])
        self.assertFalse(result["passed"])
        self.assertTrue(any("expected 180000" in f for f in result["failures"]))

    def test_fails_when_a_step_reboots_more_than_once(self):
        # A runaway loop must not read as success just because it rebooted.
        result = self._run(reported_intervals=[60000, 180000],
                           boots_after=[414, 419])
        self.assertFalse(result["passed"])
        self.assertTrue(any("expected one" in f for f in result["failures"]))


if __name__ == "__main__":
    unittest.main()
