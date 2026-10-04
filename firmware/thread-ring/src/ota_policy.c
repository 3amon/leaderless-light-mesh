#include "ota_policy.h"

#include <string.h>

void ota_policy_init(struct ota_policy *policy)
{
	memset(policy, 0, sizeof(*policy));
}

struct ota_policy_decision ota_policy_update(
	struct ota_policy *policy,
	const struct ota_policy_observation *observation)
{
	struct ota_policy_decision decision = {
		.infrastructure_ok = observation->attached &&
				     observation->infrastructure_visible,
		.record_attached = observation->attached,
	};

	decision.rollback_due = !observation->image_confirmed &&
		observation->rollback_timeout_ms >= 0 &&
		observation->now_ms > observation->rollback_timeout_ms;
	if (!decision.infrastructure_ok) {
		return decision;
	}

	policy->ever_infrastructure = true;
	decision.record_infrastructure = true;
	if (observation->image_confirmed) {
		decision.record_confirmed = true;
	} else if (!decision.rollback_due &&
		   observation->now_ms >= policy->next_confirmation_ms) {
		decision.confirmation_due = true;
		policy->next_confirmation_ms =
			observation->now_ms > INT64_MAX - OTA_CONFIRM_RETRY_MS
				? INT64_MAX
				: observation->now_ms + OTA_CONFIRM_RETRY_MS;
	}
	return decision;
}
