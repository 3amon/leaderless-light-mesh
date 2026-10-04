/* The frame power cap exists because current tracks the SUM of every channel
 * on the strip, not how bright a single pixel looks. The accent lit all 16
 * LEDs at once and measured ~6x the idle load in a 370ms burst, which sagged
 * the module-VCC rail and turned the ring white. These tests pin the two
 * properties that matter: the cap actually bounds the load, and it never
 * blanks a frame. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "led_budget.h"

/* Load figures measured from the real renderer on a 16-LED ring at 45%
 * brightness, so the thresholds here are hardware truth rather than invention. */
#define MEASURED_IDLE      488U
#define MEASURED_OLD_PEAK 2959U   /* every LED driven to one value */
#define MEASURED_NEW_PEAK 1978U   /* proportional swell */

static uint32_t apply(uint32_t load, uint32_t budget)
{
	return (load * led_budget_scale_q8(load, budget)) >> 8;
}

static void test_total(void)
{
	assert(led_budget_total(16, 90) == 1440);
	assert(led_budget_total(7, 90) == 630);
	assert(led_budget_total(100, 90) == 9000);
	/* a disabled budget or an absent strip yields no cap */
	assert(led_budget_total(16, LED_BUDGET_DISABLED) == LED_BUDGET_DISABLED);
	assert(led_budget_total(0, 90) == LED_BUDGET_DISABLED);
	assert(led_budget_total(-1, 90) == LED_BUDGET_DISABLED);
}

static void test_under_budget_is_untouched(void)
{
	uint32_t budget = led_budget_total(16, LED_BUDGET_PER_LED_DEFAULT);

	/* The ordinary scene must not be dimmed at all — the cap is for the
	 * heavy frames only. */
	assert(MEASURED_IDLE < budget);
	assert(led_budget_scale_q8(MEASURED_IDLE, budget) == 256U);
	assert(led_budget_scale_q8(budget, budget) == 256U);
	assert(led_budget_scale_q8(0, budget) == 256U);
}

static void test_over_budget_is_bounded(void)
{
	uint32_t budget = led_budget_total(16, LED_BUDGET_PER_LED_DEFAULT);

	/* Both accent peaks exceed the rail budget and must come back under it.
	 * Integer truncation can only ever land at or below the target. */
	assert(MEASURED_OLD_PEAK > budget && MEASURED_NEW_PEAK > budget);
	assert(apply(MEASURED_OLD_PEAK, budget) <= budget);
	assert(apply(MEASURED_NEW_PEAK, budget) <= budget);

	/* The worse offender is scaled harder, which is the point. */
	assert(led_budget_scale_q8(MEASURED_OLD_PEAK, budget) <
	       led_budget_scale_q8(MEASURED_NEW_PEAK, budget));
}

static void test_never_blanks_and_never_overflows(void)
{
	/* A frame is dimmed, never blanked: losing the pattern outright is
	 * worse than sitting slightly over budget on a very dark strip. */
	assert(led_budget_scale_q8(1000000U, 1U) >= 1U);

	/* 300 LEDs at full white overflows a naive 32-bit load*256. */
	uint32_t huge = 300U * 765U;
	uint32_t budget = led_budget_total(300, LED_BUDGET_PER_LED_DEFAULT);

	assert(led_budget_scale_q8(huge, budget) > 0U);
	assert(apply(huge, budget) <= budget);
}

static void test_disabled_cap_passes_everything(void)
{
	assert(led_budget_scale_q8(MEASURED_OLD_PEAK, LED_BUDGET_DISABLED) == 256U);
}

static void test_slew_is_fast_attack_slow_release(void)
{
	const uint32_t step = LED_BUDGET_RELEASE_STEP_Q8;

	/* Attack is immediate: the rail cannot wait for a ramp. */
	assert(led_budget_slew_q8(256U, 100U, step) == 100U);
	assert(led_budget_slew_q8(200U, 1U, step) == 1U);

	/* Release is ramped, so a scene whose load straddles the ceiling does
	 * not alternate between scaled and unscaled frames — that stepping is
	 * what reads as choppiness on the ring. */
	uint32_t s = led_budget_slew_q8(100U, 256U, step);

	assert(s > 100U && s < 256U);
	assert(s == 100U + step);

	/* and it converges rather than oscillating or overshooting */
	uint32_t prev = 0;

	s = 100U;
	for (int i = 0; i < 200 && s < 256U; i++) {
		prev = s;
		s = led_budget_slew_q8(s, 256U, step);
		assert(s > prev);
		assert(s <= 256U);
	}
	assert(s == 256U);

	/* a zero step degenerates to instant, never to a stall */
	assert(led_budget_slew_q8(100U, 256U, 0U) == 256U);
}

int main(void)
{
	test_total();
	test_under_budget_is_untouched();
	test_over_budget_is_bounded();
	test_never_blanks_and_never_overflows();
	test_disabled_cap_passes_everything();
	test_slew_is_fast_attack_slow_release();
	printf("led budget tests passed\n");
	return 0;
}
