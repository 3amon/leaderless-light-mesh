#include <assert.h>
#include <stdio.h>

#include "heal_policy.h"

#define MIN_MS(m) ((int64_t)(m) * 60000)

static struct heal_observation alone(int64_t lonely_for_ms, uint8_t attempts)
{
	return (struct heal_observation) {
		.lonely_for_ms = lonely_for_ms,
		.attempts = attempts,
		.lonely = true,
		.power_on_reset = false,
	};
}

static void test_interval_schedule_and_clamp(void)
{
	assert(heal_policy_interval_ms(0) == MIN_MS(1));
	assert(heal_policy_interval_ms(1) == MIN_MS(3));
	assert(heal_policy_interval_ms(2) == MIN_MS(10));
	assert(heal_policy_interval_ms(3) == MIN_MS(30));

	/* Past the end of the table it must clamp, not index off it. */
	assert(heal_policy_interval_ms(4) == MIN_MS(30));
	assert(heal_policy_interval_ms(200) == MIN_MS(30));
	assert(heal_policy_interval_ms(255) == MIN_MS(30));
}

static void test_waits_for_the_full_interval(void)
{
	struct heal_observation input = alone(MIN_MS(1) - 1, 0);

	/* One millisecond short of the first minute: still waiting. */
	assert(!heal_policy_update(&input).reboot);

	input.lonely_for_ms = MIN_MS(1);
	struct heal_decision d = heal_policy_update(&input);

	assert(d.reboot);
	assert(d.attempts == 1);
}

/* The whole point of the backoff: each rejoin must wait longer than the last.
 * This is the sequence a ring alone all night should walk, once. */
static void test_escalates_across_consecutive_reboots(void)
{
	const int64_t expected[] = {
		MIN_MS(1), MIN_MS(3), MIN_MS(10), MIN_MS(30), MIN_MS(30),
	};
	uint8_t attempts = 0;

	for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
		struct heal_observation input = alone(expected[i] - 1, attempts);

		/* Still short of this step's interval. */
		assert(!heal_policy_update(&input).reboot);

		input.lonely_for_ms = expected[i];
		struct heal_decision d = heal_policy_update(&input);

		assert(d.reboot);
		assert(d.interval_ms == expected[i]);
		assert(d.attempts == attempts + 1);

		/* A reboot follows: the count is persisted and solitude restarts.
		 * Recovering `attempts` here is exactly what the firmware must do
		 * across the reset -- holding it in RAM lost it and pinned the
		 * interval at the first step forever. */
		attempts = d.attempts;
	}
	assert(attempts == 5);
}

static void test_company_resets_the_backoff(void)
{
	struct heal_observation input = alone(MIN_MS(90), 3);

	input.lonely = false;
	struct heal_decision d = heal_policy_update(&input);

	assert(!d.reboot);
	assert(d.attempts == 0);
	assert(d.interval_ms == MIN_MS(1));

	/* ...and the next stranding starts from the fast retry again: one
	 * millisecond short of a minute still waits, a full minute fires. */
	struct heal_observation next = alone(MIN_MS(1) - 1, 0);

	assert(!heal_policy_update(&next).reboot);
	next.lonely_for_ms = MIN_MS(1);
	assert(heal_policy_update(&next).reboot);
}

static void test_power_on_clears_inherited_backoff(void)
{
	/* Persisted state survives a power cycle, so without this a ring
	 * switched on in the morning would inherit last night's 30 min wait. */
	struct heal_observation input = alone(MIN_MS(5), 3);

	input.power_on_reset = true;
	struct heal_decision d = heal_policy_update(&input);

	assert(!d.reboot);
	assert(d.attempts == 0);
	assert(d.interval_ms == MIN_MS(1));
}

static void test_not_yet_lonely_never_reboots(void)
{
	/* Not lonely at all, however long ago something happened. */
	struct heal_observation input = alone(MIN_MS(600), 3);

	input.lonely = false;
	assert(!heal_policy_update(&input).reboot);

	/* Lonely, but the run has only just begun. */
	input.lonely = true;
	input.lonely_for_ms = 0;
	assert(!heal_policy_update(&input).reboot);
}

static void test_attempt_count_saturates(void)
{
	struct heal_observation input = alone(MIN_MS(600), 255);
	struct heal_decision d = heal_policy_update(&input);

	assert(d.reboot);
	assert(d.attempts == 255);   /* must not wrap back to a fast retry */
}

static void test_solo_install_never_restarts(void)
{
	/* A deliberate single-node install is permanently lonely, so the
	 * backoff would otherwise restart it forever: every reboot is certain
	 * to find the same solitude. The flag must suppress the restart no
	 * matter how long the solitude has lasted or how far the backoff has
	 * escalated. */
	for (uint8_t attempts = 0; attempts < 6; attempts++) {
		struct heal_observation obs = {
			.lonely = true,
			.lonely_for_ms = 24 * 60 * 60 * 1000LL,
			.attempts = attempts,
			.power_on_reset = false,
			.solo_expected = true,
		};
		struct heal_decision d = heal_policy_update(&obs);

		assert(!d.reboot);
		/* the counter is preserved, so clearing the flag resumes the
		 * backoff instead of granting a fresh burst of fast retries */
		assert(d.attempts == attempts);
	}

	/* And it changes nothing else: with the flag clear, the same
	 * observation still restarts as before. */
	struct heal_observation fleet = {
		.lonely = true,
		.lonely_for_ms = 24 * 60 * 60 * 1000LL,
		.attempts = 2,
		.power_on_reset = false,
		.solo_expected = false,
	};
	struct heal_decision d = heal_policy_update(&fleet);

	assert(d.reboot);
	assert(d.attempts == 3);
}

int main(void)
{
	test_solo_install_never_restarts();
	test_interval_schedule_and_clamp();
	test_waits_for_the_full_interval();
	test_escalates_across_consecutive_reboots();
	test_company_resets_the_backoff();
	test_power_on_clears_inherited_backoff();
	test_not_yet_lonely_never_reboots();
	test_attempt_count_saturates();
	puts("heal policy tests passed");
	return 0;
}
