import importlib.util
import pathlib
import subprocess
import sys
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
PI_TOOLS = ROOT / "pi-tools"
sys.path.insert(0, str(PI_TOOLS))
SPEC = importlib.util.spec_from_file_location("deploy", PI_TOOLS / "deploy.py")
DEPLOY = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = DEPLOY
SPEC.loader.exec_module(DEPLOY)


BASE_HASH = "AA" * 32
CANDIDATE_HASH = "BB" * 32


def state(
    slot,
    version,
    image_hash,
    *,
    active=False,
    confirmed=False,
    pending=False,
):
    return {
        "slot": slot,
        "version": version,
        "hash": image_hash,
        "active": active,
        "confirmed": confirmed,
        "pending": pending,
        "bootable": True,
        "permanent": False,
    }


BASELINE = [
    state(0, "0.1.10", BASE_HASH, active=True, confirmed=True),
    state(1, "0.1.8", "CC" * 32),
]
UPLOADED = [
    state(0, "0.1.10", BASE_HASH, active=True, confirmed=True),
    state(1, "0.1.12", CANDIDATE_HASH),
]
ACTIVE_UNCONFIRMED = [
    state(0, "0.1.12", CANDIDATE_HASH, active=True),
    state(1, "0.1.10", BASE_HASH, confirmed=True),
]
ACTIVE_CONFIRMED = [
    state(0, "0.1.12", CANDIDATE_HASH, active=True, confirmed=True),
    state(1, "0.1.10", BASE_HASH),
]
ROLLED_BACK = [
    state(0, "0.1.10", BASE_HASH, active=True, confirmed=True),
    state(1, "0.1.12", CANDIDATE_HASH),
]
HEALTHY = {
    "name": "opal-reef-tide",
    "infra": 1,
    "up": 100,
    "reset": "00000002/ota",
}


class Clock:
    def __init__(self):
        self.now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class Events:
    def __init__(self):
        self.records = []

    def emit(self, event, **fields):
        self.records.append((event, fields))


class FakeBackend:
    def __init__(
        self, *, statuses, states, upload_errors=None, update_reply="update rc=0 armed, rebooting"
    ):
        self._statuses = list(statuses)
        self._states = list(states)
        self.upload_errors = list(upload_errors or [])
        self.update_reply = update_reply
        self.upload_calls = 0
        self.update_calls = 0

    @staticmethod
    def _next(sequence):
        if not sequence:
            raise AssertionError("fake backend sequence exhausted")
        value = sequence.pop(0)
        if isinstance(value, Exception):
            raise value
        return value

    def status(self, device, timeout):
        return self._next(self._statuses)

    def states(self, device, timeout):
        return self._next(self._states)

    def upload(self, device, image, timeout):
        self.upload_calls += 1
        if self.upload_errors:
            raise self.upload_errors.pop(0)

    def update(self, device, timeout):
        self.update_calls += 1
        if isinstance(self.update_reply, Exception):
            raise self.update_reply
        return self.update_reply


class DeploymentTests(unittest.TestCase):
    def setUp(self):
        self.device = DEPLOY.Device("opal-reef-tide", "fd00::1", "SERIAL", True, 0)
        self.image = DEPLOY.ImageMetadata(
            pathlib.Path("candidate.bin"),
            DEPLOY.Version(0, 1, 12),
            CANDIDATE_HASH,
            "dd" * 32,
            1234,
            512,
            700,
        )
        self.clock = Clock()
        self.events = Events()
        self.config = DEPLOY.DeployConfig(
            command_timeout=0.1,
            state_timeout=0.1,
            upload_timeout=1,
            upload_attempts=3,
            upload_retry_delay=1,
            upload_verify_timeout=2,
            reboot_timeout=3,
            poll_interval=1,
        )

    def run_deploy(self, backend):
        return DEPLOY.deploy_transaction(
            self.device,
            self.image,
            backend,
            self.events,
            self.config,
            sleep=self.clock.sleep,
            monotonic=self.clock.monotonic,
        )

    def run_stage(self, backend):
        return DEPLOY.stage_transaction(
            self.device,
            self.image,
            backend,
            self.events,
            self.config,
            sleep=self.clock.sleep,
            monotonic=self.clock.monotonic,
        )

    def assert_classification(self, expected, backend):
        with self.assertRaises(DEPLOY.DeploymentFailure) as caught:
            self.run_deploy(backend)
        self.assertEqual(caught.exception.classification, expected)
        return caught.exception

    def test_upload_failure_never_arms(self):
        backend = FakeBackend(
            statuses=[HEALTHY],
            states=[BASELINE],
            upload_errors=[RuntimeError("cut") for _ in range(3)],
        )
        self.assert_classification("upload_failed", backend)
        self.assertEqual(backend.upload_calls, 3)
        self.assertEqual(backend.update_calls, 0)

    def test_stage_uploads_and_verifies_without_arming(self):
        backend = FakeBackend(
            statuses=[HEALTHY, HEALTHY],
            states=[BASELINE, UPLOADED, UPLOADED],
        )
        result = self.run_stage(backend)
        self.assertEqual(result["classification"], "staged")
        self.assertEqual(backend.upload_calls, 1)
        self.assertEqual(backend.update_calls, 0)

    def test_exact_inactive_candidate_is_idempotently_staged(self):
        backend = FakeBackend(
            statuses=[HEALTHY, HEALTHY],
            states=[UPLOADED, UPLOADED],
        )
        result = self.run_stage(backend)
        self.assertEqual(result["classification"], "already_staged")
        self.assertEqual(backend.upload_calls, 0)
        self.assertEqual(backend.update_calls, 0)

    def test_stage_identity_change_never_arms(self):
        backend = FakeBackend(
            statuses=[HEALTHY, None],
            states=[BASELINE, UPLOADED, UPLOADED],
        )
        with self.assertRaises(DEPLOY.DeploymentFailure) as caught:
            self.run_stage(backend)
        self.assertEqual(caught.exception.classification, "stage_preflight_failed")
        self.assertEqual(backend.update_calls, 0)

    @mock.patch.object(DEPLOY.subprocess, "run")
    def test_subprocess_backend_redacts_binary_failure_output(self, run):
        payload = b"firmware-secret-payload"
        run.return_value = subprocess.CompletedProcess(
            args=[], returncode=1, stdout="", stderr=f"smp_data={payload!r} rc=5"
        )
        backend = DEPLOY.SubprocessBackend(pathlib.Path("python"), pathlib.Path("helper"))
        with self.assertRaises(RuntimeError) as caught:
            backend._smp(self.device, 1, ["state"], 2)
        self.assertNotIn("firmware-secret", str(caught.exception))
        self.assertIn("bytes redacted", str(caught.exception))

    @mock.patch.object(DEPLOY.subprocess, "run")
    def test_subprocess_backend_uses_thread_safe_default_mtu(self, run):
        run.return_value = subprocess.CompletedProcess(
            args=[], returncode=0, stdout='{"ok": true, "images": []}', stderr=""
        )
        backend = DEPLOY.SubprocessBackend(pathlib.Path("python"), pathlib.Path("helper"))
        backend.states(self.device, 1)
        command = run.call_args.args[0]
        self.assertEqual(command[command.index("--mtu") + 1], "192")

    @mock.patch.object(DEPLOY.subprocess, "run")
    def test_subprocess_backend_honors_selected_mtu(self, run):
        run.return_value = subprocess.CompletedProcess(
            args=[], returncode=0, stdout='{"ok": true, "images": []}', stderr=""
        )
        backend = DEPLOY.SubprocessBackend(pathlib.Path("python"), pathlib.Path("helper"), mtu=256)
        backend.states(self.device, 1)
        command = run.call_args.args[0]
        self.assertEqual(command[command.index("--mtu") + 1], "256")

    @mock.patch.object(DEPLOY.subprocess, "run")
    def test_subprocess_backend_retries_upload_chunks_in_one_session(self, run):
        run.return_value = subprocess.CompletedProcess(
            args=[], returncode=0, stdout='{"ok": true}', stderr=""
        )
        backend = DEPLOY.SubprocessBackend(
            pathlib.Path("python"),
            pathlib.Path("helper"),
            chunk_attempts=4,
            upload_request_timeout=2.5,
        )
        backend.upload(self.device, pathlib.Path("candidate.bin"), 100)
        command = run.call_args.args[0]
        self.assertEqual(command[command.index("--timeout") + 1], "2.5")
        self.assertEqual(command[command.index("--chunk-attempts") + 1], "4")

    @mock.patch.object(DEPLOY.control_client, "authenticated_udp_command")
    def test_subprocess_backend_uses_authenticated_update_by_default(self, command):
        command.return_value = "update rc=0 armed, rebooting"
        key_dir = pathlib.Path("/secure/keys")
        backend = DEPLOY.SubprocessBackend(
            pathlib.Path("python"), pathlib.Path("helper"), control_key_dir=key_dir
        )
        self.assertEqual(backend.update(self.device, 0.5), "update rc=0 armed, rebooting")
        command.assert_called_once_with(
            self.device.address,
            self.device.name,
            "update",
            0.5,
            key_path=key_dir / f"{self.device.name}.key",
        )

    def test_stale_slot_never_arms(self):
        backend = FakeBackend(
            statuses=[HEALTHY],
            states=[BASELINE, BASELINE, BASELINE],
        )
        self.assert_classification("stale_slot", backend)
        self.assertEqual(backend.update_calls, 0)

    def test_delayed_state_update_can_succeed(self):
        backend = FakeBackend(
            statuses=[HEALTHY, HEALTHY, None, HEALTHY],
            states=[BASELINE, BASELINE, UPLOADED, UPLOADED, ACTIVE_CONFIRMED],
        )
        result = self.run_deploy(backend)
        self.assertEqual(result["classification"], "success")
        self.assertTrue(result["saw_down"])
        self.assertEqual(backend.update_calls, 1)

    def test_lost_authenticated_ack_reconciles_exact_confirmed_image(self):
        backend = FakeBackend(
            statuses=[HEALTHY, HEALTHY, None, HEALTHY],
            states=[BASELINE, UPLOADED, UPLOADED, ACTIVE_CONFIRMED],
            update_reply=DEPLOY.control_client.ControlAuthReplyMissing("reply lost"),
        )
        result = self.run_deploy(backend)
        self.assertEqual(result["classification"], "success")
        self.assertFalse(result["arm_acknowledged"])
        self.assertTrue(result["saw_down"])
        self.assertIn(
            "arm_acknowledgement_lost",
            [event for event, _fields in self.events.records],
        )

    def test_lost_legacy_backend_ack_reconciles_exact_confirmed_image(self):
        backend = FakeBackend(
            statuses=[HEALTHY, HEALTHY, None, HEALTHY],
            states=[BASELINE, UPLOADED, UPLOADED, ACTIVE_CONFIRMED],
            update_reply=None,
        )
        result = self.run_deploy(backend)
        self.assertEqual(result["classification"], "success")
        self.assertFalse(result["arm_acknowledged"])

    def test_exact_staged_candidate_is_not_destructively_reuploaded(self):
        backend = FakeBackend(
            statuses=[HEALTHY, HEALTHY, None, HEALTHY],
            states=[UPLOADED, UPLOADED, ACTIVE_CONFIRMED],
        )
        result = self.run_deploy(backend)
        self.assertEqual(result["classification"], "success")
        self.assertEqual(backend.upload_calls, 0)
        self.assertEqual(backend.update_calls, 1)
        self.assertIn(
            "upload_skipped",
            [event for event, _fields in self.events.records],
        )

    def test_reboot_loss_is_ambiguous(self):
        backend = FakeBackend(
            statuses=[HEALTHY, HEALTHY, None, None, None],
            states=[BASELINE, UPLOADED, UPLOADED],
        )
        failure = self.assert_classification("reboot_loss", backend)
        self.assertTrue(failure.ambiguous)

    def test_rollback_is_classified(self):
        backend = FakeBackend(
            statuses=[HEALTHY, HEALTHY, None, HEALTHY],
            states=[BASELINE, UPLOADED, UPLOADED, ROLLED_BACK],
        )
        failure = self.assert_classification("rollback", backend)
        self.assertFalse(failure.ambiguous)

    def test_confirmation_timeout_is_classified(self):
        backend = FakeBackend(
            statuses=[HEALTHY, HEALTHY, None, HEALTHY, HEALTHY],
            states=[
                BASELINE,
                UPLOADED,
                UPLOADED,
                ACTIVE_UNCONFIRMED,
                ACTIVE_UNCONFIRMED,
            ],
        )
        self.assert_classification("confirmation_timeout", backend)

    def test_same_version_requires_explicit_override(self):
        same = DEPLOY.ImageMetadata(
            self.image.path,
            DEPLOY.Version(0, 1, 10),
            CANDIDATE_HASH,
            self.image.file_sha256,
            self.image.size,
            self.image.header_size,
            self.image.payload_size,
        )
        with self.assertRaises(DEPLOY.DeploymentFailure) as caught:
            DEPLOY.validate_preflight(
                self.device,
                same,
                HEALTHY,
                BASELINE,
                allow_downgrade=False,
                allow_same_version=False,
            )
        self.assertEqual(caught.exception.classification, "version_policy")

    def test_mcuboot_four_component_state_version_matches_build_metadata(self):
        image = DEPLOY.ImageMetadata(
            self.image.path,
            DEPLOY.Version(0, 1, 22, 2),
            CANDIDATE_HASH,
            self.image.file_sha256,
            self.image.size,
            self.image.header_size,
            self.image.payload_size,
        )
        candidate = state(1, "0.1.22.2", CANDIDATE_HASH)
        self.assertTrue(DEPLOY.state_matches(candidate, image))

    def test_exact_active_image_is_idempotent_success(self):
        current = DEPLOY.ImageMetadata(
            self.image.path,
            DEPLOY.Version(0, 1, 10),
            BASE_HASH,
            self.image.file_sha256,
            self.image.size,
            self.image.header_size,
            self.image.payload_size,
        )
        backend = FakeBackend(statuses=[HEALTHY], states=[BASELINE])
        result = DEPLOY.deploy_transaction(
            self.device,
            current,
            backend,
            self.events,
            self.config,
            sleep=self.clock.sleep,
            monotonic=self.clock.monotonic,
        )
        self.assertEqual(result["classification"], "already_current")
        self.assertEqual(backend.upload_calls, 0)
        self.assertEqual(backend.update_calls, 0)

    def test_smp_mtu_is_bounded(self):
        args = DEPLOY.parse_args(["stage", "opal-reef-tide", "candidate.bin", "--smp-mtu", "192"])
        self.assertEqual(args.smp_mtu, 192)
        self.assertEqual(args.smp_chunk_attempts, 3)
        self.assertEqual(args.smp_upload_request_timeout, 3.0)
        self.assertEqual(args.upload_attempts, 8)
        for invalid in ("128", "513"):
            with self.subTest(invalid=invalid), self.assertRaises(SystemExit):
                DEPLOY.parse_args(
                    ["stage", "opal-reef-tide", "candidate.bin", "--smp-mtu", invalid]
                )

    def test_smp_chunk_attempts_are_bounded(self):
        for invalid in ("0", "9"):
            with self.subTest(invalid=invalid), self.assertRaises(SystemExit):
                DEPLOY.parse_args(
                    [
                        "stage",
                        "opal-reef-tide",
                        "candidate.bin",
                        "--smp-chunk-attempts",
                        invalid,
                    ]
                )

    def test_upload_attempts_are_bounded(self):
        for invalid in ("0", "13"):
            with self.subTest(invalid=invalid), self.assertRaises(SystemExit):
                DEPLOY.parse_args(
                    [
                        "stage",
                        "opal-reef-tide",
                        "candidate.bin",
                        "--upload-attempts",
                        invalid,
                    ]
                )


if __name__ == "__main__":
    unittest.main()
