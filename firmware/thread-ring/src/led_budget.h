/* Bound the strip's per-frame electrical load.
 *
 * The ring's WS2812s run off the module's switchable VCC rail (see
 * led_power_mode.h). Current scales with the SUM OF ALL CHANNELS across the
 * strip, not with how bright any one pixel looks, so a scene that lights every
 * LED at once is the worst case even at modest per-pixel values. The party
 * accent does exactly that: measured peak load was ~6x the idle scene in a
 * 370ms burst, which sagged the rail and turned the ring white — a brownout,
 * not a colour bug, and invisible in any pixel-level simulation.
 *
 * Scaling the whole frame by one factor keeps every pixel's ratios, so hue and
 * relative structure survive; only the overall level moves.
 */

#pragma once

#include <stdint.h>

/* Per-LED share of the budget, in summed 8-bit channel units (0-765).
 *
 * 45 was found on the hardware, not from a datasheet. The rail rendered
 * correctly at a whole-strip load of ~440 (the ring at 15% brightness) and
 * browned out to white at ~1300 (spark-rail at 45%), so the true ceiling sits
 * between those; 45/LED puts a 16-LED ring at 720, nearer the known-good end.
 * The first attempt at 90 was too permissive to ever fire on spark-rail, whose
 * peak load is 1327 — the cap existed but never engaged, and the ring still
 * went white.
 *
 * The ordinary scene measures ~30 per LED, so the cap stays out of the way and
 * only engages on the frames that would have sagged the rail. A different
 * assembly will want a different number: tune with `ringctl <ring> budget N`
 * and set what you find here, because the live value is RAM-only. */
#define LED_BUDGET_PER_LED_DEFAULT 45U

/* 0 disables the cap entirely. */
#define LED_BUDGET_DISABLED 0U

/* Total budget for a strip of `led_count` pixels. */
uint32_t led_budget_total(int led_count, uint32_t per_led);

/* Per-frame recovery step, in Q8 units, when the load drops back under budget.
 * A limiter that changes gain instantly produces visible stepping: the load of
 * an animating scene crosses the ceiling repeatedly, so the frame alternates
 * between scaled and unscaled and the ring reads as choppy. Cutting gain is
 * immediate (the rail cannot wait), but restoring it is ramped over roughly a
 * third of a second at the render loop's frame rate. */
#define LED_BUDGET_RELEASE_STEP_Q8 12U

/* Slew the applied scale toward `target`: instant attack, ramped release.
 * `current` is the scale used on the previous frame. */
uint32_t led_budget_slew_q8(uint32_t current, uint32_t target,
			    uint32_t release_step);

/* Q8 scale (256 = 1.0) to apply to every channel so `load` fits `budget`.
 * Returns 256 when already within budget, when the budget is disabled, or
 * when the frame is dark. Never returns 0 for a lit frame: a capped frame is
 * dimmed, never blanked. */
uint32_t led_budget_scale_q8(uint32_t load, uint32_t budget);
