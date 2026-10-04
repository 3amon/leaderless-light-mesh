import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "firmware/thread-ring/src/ota_policy.c"
INCLUDE = SOURCE.parent


class OtaConfirmationPolicyTests(unittest.TestCase):
    def test_confirmation_retry_and_rollback_lifecycle(self):
        program = r"""
#include <assert.h>
#include "ota_policy.h"
int main(void) {
    struct ota_policy policy;
    struct ota_policy_observation observation = {
        .rollback_timeout_ms = 300000,
    };
    struct ota_policy_decision decision;

    ota_policy_init(&policy);
    assert(policy.next_confirmation_ms == 0);
    assert(!policy.ever_infrastructure);

    decision = ota_policy_update(&policy, &observation);
    assert(!decision.infrastructure_ok);
    assert(!decision.confirmation_due);
    assert(!decision.rollback_due);

    observation.attached = true;
    observation.infrastructure_visible = false;
    decision = ota_policy_update(&policy, &observation);
    assert(decision.record_attached);
    assert(!decision.record_infrastructure);
    assert(!decision.confirmation_due);

    observation.now_ms = 1000;
    observation.infrastructure_visible = true;
    decision = ota_policy_update(&policy, &observation);
    assert(decision.infrastructure_ok);
    assert(decision.record_infrastructure);
    assert(decision.confirmation_due);
    assert(policy.ever_infrastructure);
    assert(policy.next_confirmation_ms == 1000 + OTA_CONFIRM_RETRY_MS);

    observation.now_ms = 1000 + OTA_CONFIRM_RETRY_MS - 1;
    decision = ota_policy_update(&policy, &observation);
    assert(!decision.confirmation_due);
    observation.now_ms++;
    decision = ota_policy_update(&policy, &observation);
    assert(decision.confirmation_due);

    observation.image_confirmed = true;
    decision = ota_policy_update(&policy, &observation);
    assert(decision.record_confirmed);
    assert(!decision.confirmation_due);

    observation.image_confirmed = false;
    observation.now_ms = observation.rollback_timeout_ms;
    decision = ota_policy_update(&policy, &observation);
    assert(!decision.rollback_due);
    observation.now_ms++;
    decision = ota_policy_update(&policy, &observation);
    assert(decision.rollback_due);
    assert(!decision.confirmation_due);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            test_c = path / "policy_test.c"
            executable = path / "policy_test"
            test_c.write_text(program)
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Werror",
                    f"-I{INCLUDE}",
                    str(test_c),
                    str(SOURCE),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
