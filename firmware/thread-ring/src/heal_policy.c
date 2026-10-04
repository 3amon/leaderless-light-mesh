#include "heal_policy.h"

static const int64_t HEAL_BACKOFF_MS[HEAL_POLICY_STEPS] = {
	60000,     /* 1 min  — assume a radio error, cheap to retry */
	180000,    /* 3 min */
	600000,    /* 10 min — probably actually alone */
	1800000,   /* 30 min — settle here; just render */
};

int64_t heal_policy_interval_ms(uint8_t attempts)
{
	uint8_t index = attempts < HEAL_POLICY_STEPS ? attempts
						     : HEAL_POLICY_STEPS - 1;

	return HEAL_BACKOFF_MS[index];
}

struct heal_decision heal_policy_update(const struct heal_observation *obs)
{
	struct heal_decision decision = {
		.reboot = false,
		.attempts = obs->attempts,
		.interval_ms = heal_policy_interval_ms(obs->attempts),
	};

	if (obs->power_on_reset) {
		/* Somebody just switched this on. Whatever backoff the previous
		 * situation earned does not apply to this one. */
		decision.attempts = 0;
		decision.interval_ms = heal_policy_interval_ms(0);
		return decision;
	}
	if (obs->solo_expected) {
		/* A deliberate single-node install: never restart for solitude.
		 * The attempt counter is deliberately NOT reset — clearing the
		 * flag should resume the backoff where it left off rather than
		 * grant a fresh burst of fast retries. */
		return decision;
	}
	if (!obs->lonely) {
		/* Company: the next stranding gets the fast retry again. */
		decision.attempts = 0;
		decision.interval_ms = heal_policy_interval_ms(0);
		return decision;
	}
	if (obs->lonely_for_ms < decision.interval_ms) {
		return decision;
	}

	decision.reboot = true;
	if (decision.attempts < UINT8_MAX) {
		decision.attempts++;
	}
	return decision;
}
