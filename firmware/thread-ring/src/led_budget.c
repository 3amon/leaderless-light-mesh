#include "led_budget.h"

uint32_t led_budget_total(int led_count, uint32_t per_led)
{
	if (led_count <= 0 || per_led == LED_BUDGET_DISABLED) {
		return LED_BUDGET_DISABLED;
	}
	return (uint32_t)led_count * per_led;
}

uint32_t led_budget_scale_q8(uint32_t load, uint32_t budget)
{
	if (budget == LED_BUDGET_DISABLED || load <= budget) {
		return 256U;
	}
	/* 64-bit intermediate: a 300-LED strip at full white loads ~229k, which
	 * overflows a 32-bit multiply by 256 well before the divide. */
	uint32_t scale = (uint32_t)(((uint64_t)budget * 256U) / load);

	/* A frame over budget is dimmed, never blanked — losing the pattern
	 * entirely is worse than running slightly over on a very dark strip. */
	return scale == 0U ? 1U : scale;
}

uint32_t led_budget_slew_q8(uint32_t current, uint32_t target,
			    uint32_t release_step)
{
	if (target <= current) {
		/* Attack is instant: the rail is the constraint and it does not
		 * get to wait for a ramp. */
		return target;
	}
	if (release_step == 0U || current + release_step >= target) {
		return target;
	}
	return current + release_step;
}
