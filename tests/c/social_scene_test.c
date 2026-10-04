#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "social_scene.h"

#ifndef TEST_LED_COUNT
#define TEST_LED_COUNT 16
#endif

static void assert_color(struct frgb color)
{
	assert(isfinite(color.r));
	assert(isfinite(color.g));
	assert(isfinite(color.b));
	assert(color.r >= 0.f && color.r <= 1.f);
	assert(color.g >= 0.f && color.g <= 1.f);
	assert(color.b >= 0.f && color.b <= 1.f);
}

static void assert_same_color(struct frgb first, struct frgb second)
{
	assert(first.r == second.r);
	assert(first.g == second.g);
	assert(first.b == second.b);
}

static float hue_distance(float first, float second)
{
	float distance = fabsf(fmodf(first - second, 360.f));

	return distance > 180.f ? 360.f - distance : distance;
}

static float color_saturation(struct frgb color)
{
	float maximum = fmaxf(color.r, fmaxf(color.g, color.b));
	float minimum = fminf(color.r, fminf(color.g, color.b));

	return maximum > 0.f ? (maximum - minimum) / maximum : 0.f;
}

static void test_plan_contract(void)
{
	assert(social_tier_for_member_count(0) == SOCIAL_SOLO);
	assert(social_tier_for_member_count(1) == SOCIAL_SOLO);
	assert(social_tier_for_member_count(2) == SOCIAL_DUO);
	assert(social_tier_for_member_count(3) == SOCIAL_CROWD);
	assert(social_tier_for_member_count(255) == SOCIAL_CROWD);

	for (uint32_t seed = 1; seed < 1000; seed += 19) {
		struct social_scene solo;
		struct social_scene duo;
		struct social_scene crowd;
		struct social_scene_plan first;
		struct social_scene_plan second;

		social_scene_init(&solo, seed, SOCIAL_SOLO, 99);
		social_scene_init(&duo, seed, SOCIAL_DUO, 1);
		social_scene_init(&crowd, seed, SOCIAL_CROWD, 4);

		social_scene_plan_at(&solo, 17.f, &first);
		social_scene_plan_at(&solo, 17.f, &second);
		assert(first.family == second.family);
		assert(first.hue[0] == second.hue[0]);
		assert(first.color_count == 1);
		assert(first.member_count == 1);
		assert(first.period_s >= 10.f && first.period_s <= 20.f);

		social_scene_plan_at(&duo, 17.f, &first);
		assert(first.color_count == 2);
		assert(first.member_count == 2);
		assert(first.period_s >= 3.5f && first.period_s <= 7.f);
		assert(hue_distance(first.hue[0], first.hue[1]) >= 60.f);

		social_scene_plan_at(&crowd, 17.f, &first);
		assert(first.color_count == 4);
		assert(first.member_count == 4);
		assert(first.period_s >= 1.25f && first.period_s <= 2.75f);
	}
}

static void test_motion_tiers_are_visibly_separated(void)
{
	for (uint32_t seed = 1; seed < 1000; seed += 19) {
		struct social_scene solo;
		struct social_scene duo;
		struct social_scene crowd;
		struct social_scene_plan solo_plan;
		struct social_scene_plan duo_plan;
		struct social_scene_plan crowd_plan;

		social_scene_init(&solo, seed, SOCIAL_SOLO, 1);
		social_scene_init(&duo, seed, SOCIAL_DUO, 2);
		social_scene_init(&crowd, seed, SOCIAL_CROWD, 3);
		for (int epoch = -5; epoch <= 20; epoch++) {
			float time = (float)epoch * 60.f + 17.f;

			social_scene_plan_at(&solo, time, &solo_plan);
			social_scene_plan_at(&duo, time, &duo_plan);
			social_scene_plan_at(&crowd, time, &crowd_plan);

			assert(duo_plan.period_s < solo_plan.period_s);
			assert(crowd_plan.period_s < duo_plan.period_s);
		}
	}
}

static void test_crowd_palette_caps_at_five(void)
{
	struct social_scene scene;
	struct social_scene_plan plan;

	social_scene_init(&scene, 0x12345678U, SOCIAL_CROWD, 17);
	social_scene_plan_at(&scene, 20.f, &plan);
	assert(plan.color_count == SOCIAL_SCENE_MAX_COLORS);
	assert(plan.member_count == 17);
}

static void test_rendering_is_bounded_repeatable_and_synchronized(void)
{
	const float times[] = {-0.1f, 0.f, 4.999f, 5.f, 17.25f,
			       59.999f, 60.f, 60.001f, 3599.5f};

	for (int tier = SOCIAL_SOLO; tier <= SOCIAL_CROWD; tier++) {
		for (uint32_t seed = 1; seed < 64; seed++) {
			struct social_scene first;
			struct social_scene second;

			social_scene_init(&first, seed, (enum social_tier)tier,
					  tier + 1);
			social_scene_init(&second, seed, (enum social_tier)tier,
					  tier + 1);
			for (size_t time = 0;
			     time < sizeof(times) / sizeof(times[0]); time++) {
				for (int led = 0; led < TEST_LED_COUNT; led++) {
					struct frgb a =
						social_scene_render(&first,
								    times[time],
								    led,
								    TEST_LED_COUNT);
					struct frgb b =
						social_scene_render(&second,
								    times[time],
								    led,
								    TEST_LED_COUNT);

					assert_color(a);
					assert_same_color(a, b);
				}
			}
		}
	}
}

static void test_minute_boundary_is_continuous(void)
{
	/* Match the spatial travel of the original 16-pixel test. Longer layouts
	 * traverse more pixel positions per millisecond without introducing a
	 * discontinuity. */
	const float epsilon = 0.001f * 16.f / (float)TEST_LED_COUNT;

	for (int tier = SOCIAL_SOLO; tier <= SOCIAL_CROWD; tier++) {
		struct social_scene scene;

		social_scene_init(&scene, 0x89abcdefU,
				  (enum social_tier)tier, tier + 1);
		for (int led = 0; led < TEST_LED_COUNT; led++) {
			struct frgb before =
				social_scene_render(&scene, 60.f - epsilon, led,
							    TEST_LED_COUNT);
			struct frgb after =
				social_scene_render(&scene, 60.f + epsilon, led,
						    TEST_LED_COUNT);
			float delta = fabsf(before.r - after.r) +
				      fabsf(before.g - after.g) +
				      fabsf(before.b - after.b);

			assert(delta < 0.02f);
		}
	}
}

static void test_rendering_never_washes_to_pastel(void)
{
	for (int tier = SOCIAL_SOLO; tier <= SOCIAL_CROWD; tier++) {
		for (uint32_t seed = 1; seed < 128; seed += 7) {
			struct social_scene scene;

			social_scene_init(&scene, seed, (enum social_tier)tier,
					  tier + 1);
			for (float time = -5.f; time <= 185.f; time += 0.5f) {
				for (int led = 0; led < TEST_LED_COUNT; led++) {
					struct frgb color = social_scene_render(
						&scene, time, led, TEST_LED_COUNT);

					assert(color_saturation(color) >=
					       SOCIAL_SCENE_MIN_SATURATION -
						       0.00001f);
				}
			}
		}
	}
}

static void test_composite_vivid_guard_handles_gray(void)
{
	const struct frgb washed[] = {
		{0.6f, 0.58f, 0.55f},
		{0.7f, 0.7f, 0.7f},
		{1.2f, 1.1f, 1.05f},
	};

	for (size_t i = 0; i < sizeof(washed) / sizeof(washed[0]); i++) {
		struct frgb color =
			social_scene_vivid(washed[i], 205.f);

		assert_color(color);
		assert(color_saturation(color) >=
		       SOCIAL_SCENE_MIN_SATURATION - 0.00001f);
	}
}

static void test_crowd_remains_vivid_through_diffuser(void)
{
	for (uint8_t members = 3; members <= SOCIAL_SCENE_MAX_COLORS;
	     members++) {
		for (uint32_t seed = 1; seed < 128; seed += 7) {
			struct social_scene scene;

			social_scene_init(&scene, seed, SOCIAL_CROWD, members);
			for (float time = -5.f; time <= 185.f;
			     time += 0.5f) {
				struct frgb average = {0};

				for (int led = 0; led < TEST_LED_COUNT; led++) {
					struct frgb color = social_scene_render(
						&scene, time, led, TEST_LED_COUNT);

					average.r += color.r / (float)TEST_LED_COUNT;
					average.g += color.g / (float)TEST_LED_COUNT;
					average.b += color.b / (float)TEST_LED_COUNT;
				}
				float saturation = color_saturation(average);

				if (saturation <
				    SOCIAL_SCENE_MIN_DIFFUSED_SATURATION -
					    0.00001f) {
					fprintf(stderr,
						"diffused crowd washed out: members=%u seed=%u time=%.1f saturation=%.3f\n",
						members, seed, (double)time,
						(double)saturation);
					assert(false);
				}
			}
		}
	}
}

static void test_pattern_is_different_every_adjacent_minute(void)
{
	for (int tier = SOCIAL_SOLO; tier <= SOCIAL_CROWD; tier++) {
		struct social_scene scene;

		social_scene_init(&scene, 0x1234abcdU,
				  (enum social_tier)tier, tier + 1);
		int previous =
			social_scene_pattern_id(&scene, -5.f * 60.f + 1.f);

		for (int minute = -4; minute <= 20; minute++) {
			int current = social_scene_pattern_id(
				&scene, minute * 60.f + 1.f);

			assert(current != previous);
			previous = current;
		}
	}
}

static void test_party_mode_is_distinct_fast_vivid_and_bounded(void)
{
	for (int tier = SOCIAL_SOLO; tier <= SOCIAL_CROWD; tier++) {
		struct social_scene normal;
		struct social_scene party;

		social_scene_init(&normal, 0x51a7c0deU,
				  (enum social_tier)tier, tier + 1);
		party = normal;
		social_scene_set_party(&party, true);
		for (float time = 7.f; time < 187.f; time += 13.f) {
			struct social_scene_plan normal_plan;
			struct social_scene_plan party_plan;

			social_scene_plan_at(&normal, time, &normal_plan);
			social_scene_plan_at(&party, time, &party_plan);
			assert(!normal_plan.party && party_plan.party);
			assert(party_plan.period_s < normal_plan.period_s);
			assert(party_plan.saturation >= 0.94f);
			assert(social_scene_pattern_id(&party, time) >= 9);
			assert(strcmp(social_scene_family_name_for_mode(
				true, party_plan.tier, party_plan.family),
				social_scene_family_name(party_plan.tier,
						 party_plan.family)) != 0);
			for (int led = 0; led < TEST_LED_COUNT; led++) {
				struct frgb color = social_scene_render(
					&party, time, led, TEST_LED_COUNT);

				assert_color(color);
				assert(color_saturation(color) >=
				       SOCIAL_SCENE_MIN_SATURATION - 0.00001f);
			}
		}
	}
}

static void test_party_flash_is_sparse_soft_synchronized_and_bright(void)
{
	const float step = 0.005f;
	const float minimum_dark_gap =
		SOCIAL_SCENE_PARTY_FLASH_PERIOD_MIN_S -
		SOCIAL_SCENE_PARTY_FLASH_ACTIVE_S - 2.f * step;

	for (int tier = SOCIAL_SOLO; tier <= SOCIAL_CROWD; tier++) {
		for (uint32_t seed = 1; seed < 64; seed += 11) {
			struct social_scene normal;
			struct social_scene party;
			float previous = 0.f;
			float last_end = -100.f;
			float peak_time = 0.f;
			float peak = 0.f;
			bool active = false;
			int flashes = 0;

			social_scene_init(&normal, seed,
					  (enum social_tier)tier, tier + 1);
			party = normal;
			social_scene_set_party(&party, true);
			for (int sample = 0; sample < (int)(120.f / step);
			     sample++) {
				float time = (float)sample * step;
				float strength = social_scene_party_flash_strength(
					&party, time);

				assert(social_scene_party_flash_strength(
					&normal, time) == 0.f);
				assert(strength >= 0.f && strength <= 1.f);
				/* A 90 ms smooth attack is visible but cannot make a
				 * single-frame hard edge at the renderer cadence. */
				if (sample > 0) {
					assert(fabsf(strength - previous) < 0.1f);
				}
				if (strength > peak) {
					peak = strength;
					peak_time = time;
				}
				if (strength > 0.f && !active) {
					if (flashes > 0) {
						assert(time - last_end >=
						       minimum_dark_gap);
					}
					active = true;
					flashes++;
				} else if (strength == 0.f && active) {
					active = false;
					last_end = time;
				}
				previous = strength;
			}
			assert(flashes >= 19 && flashes <= 30);
			assert(peak > 0.99f);
			/* This used to assert EVERY led exceeded 0.94 at the
			 * peak, which was the flattening bug written down as
			 * the spec: only a scene with all its structure driven
			 * out can satisfy it. The accent's real contract is
			 * that the ring gets brighter and stays vivid, which
			 * is what is checked now; contrast preservation has
			 * its own test below. */
			float brightest = 0.f;

			for (int led = 0; led < TEST_LED_COUNT; led++) {
				struct frgb color = social_scene_render(
					&party, peak_time, led, TEST_LED_COUNT);

				assert_color(color);
				brightest = fmaxf(brightest,
						  fmaxf(color.r,
							fmaxf(color.g,
							      color.b)));
				assert(color_saturation(color) >=
				       SOCIAL_SCENE_MIN_SATURATION - 0.00001f);
			}
			assert(brightest > 0.94f);
		}
	}
}

/* The accent setting scales the party swell without touching party's
 * faster, denser motion — accent 0 must leave a party scene that still
 * moves like party, just without the bright synchronized pop. */
static void test_party_accent_scales_and_can_be_disabled(void)
{
	const float step = 0.005f;

	for (int tier = SOCIAL_SOLO; tier <= SOCIAL_CROWD; tier++) {
		struct social_scene full;
		struct social_scene half;
		struct social_scene off;
		float peak_full = 0.f;
		float peak_time = 0.f;

		social_scene_init(&full, 7u, (enum social_tier)tier, tier + 1);
		social_scene_set_party(&full, true);
		half = full;
		off = full;
		social_scene_set_accent(&half, 0.5f);
		social_scene_set_accent(&off, 0.f);

		/* social_scene_init must default to the historical swell */
		assert(full.party_accent == 1.f);

		for (int sample = 0; sample < (int)(60.f / step); sample++) {
			float time = (float)sample * step;
			float strength =
				social_scene_party_flash_strength(&full, time);

			if (strength > peak_full) {
				peak_full = strength;
				peak_time = time;
			}
		}
		assert(peak_full > 0.99f);

		/* Summed ring value, not a per-LED max: the base party scene
		 * already has bright pixels of its own, so only the TOTAL light
		 * the accent adds is a real measure of its strength. */
		float sum_full = 0.f;
		float sum_half = 0.f;
		float sum_off = 0.f;

		for (int led = 0; led < TEST_LED_COUNT; led++) {
			struct frgb a = social_scene_render(&full, peak_time,
							    led, TEST_LED_COUNT);
			struct frgb b = social_scene_render(&half, peak_time,
							    led, TEST_LED_COUNT);
			struct frgb c = social_scene_render(&off, peak_time,
							    led, TEST_LED_COUNT);

			assert_color(a);
			assert_color(b);
			assert_color(c);
			sum_full += fmaxf(a.r, fmaxf(a.g, a.b));
			sum_half += fmaxf(b.r, fmaxf(b.g, b.b));
			sum_off += fmaxf(c.r, fmaxf(c.g, c.b));
		}
		/* strictly monotonic in the accent setting */
		assert(sum_full > sum_half);
		assert(sum_half > sum_off);

		/* accent off must NOT turn party back into normal */
		struct social_scene_plan off_plan;

		social_scene_plan_at(&off, peak_time, &off_plan);
		assert(off_plan.party);
	}
}

/* The accent must SWELL the scene, not flatten it.
 *
 * Regression: the accent used to pull every pixel toward one absolute target,
 * so a dim pixel rose ~48x while an already-bright one barely moved, and at
 * full strength they all converged on that target -- measured ring contrast
 * collapsed from 7.2:1 to 1.0:1. A uniformly lit ring has no structure left
 * for the eye to read hue from, so the "soft accent" read as a white strobe
 * through a diffuser. Brightness alone cannot catch this; contrast can. */
static void test_party_accent_preserves_contrast(void)
{
	const float step = 0.005f;

	for (int tier = SOCIAL_SOLO; tier <= SOCIAL_CROWD; tier++) {
		struct social_scene base;
		float peak = 0.f;
		float peak_time = 0.f;

		social_scene_init(&base, 11u, (enum social_tier)tier, tier + 1);
		social_scene_set_party(&base, true);
		for (int sample = 0; sample < (int)(60.f / step); sample++) {
			float time = (float)sample * step;
			float strength =
				social_scene_party_flash_strength(&base, time);

			if (strength > peak) {
				peak = strength;
				peak_time = time;
			}
		}
		assert(peak > 0.99f);

		float contrast[3];
		float mean[3];
		const float accents[3] = {0.f, 0.25f, 1.f};

		for (int i = 0; i < 3; i++) {
			struct social_scene scene = base;
			float low = 1e9f;
			float high = 0.f;
			float total = 0.f;

			social_scene_set_accent(&scene, accents[i]);
			for (int led = 0; led < TEST_LED_COUNT; led++) {
				struct frgb c = social_scene_render(
					&scene, peak_time, led, TEST_LED_COUNT);
				float v = fmaxf(c.r, fmaxf(c.g, c.b));

				assert_color(c);
				low = fminf(low, v);
				high = fmaxf(high, v);
				total += v;
			}
			assert(low > 0.f);
			contrast[i] = high / low;
			mean[i] = total / (float)TEST_LED_COUNT;
		}
		/* the accent brightens the ring... */
		assert(mean[1] > mean[0]);
		assert(mean[2] > mean[1]);
		/* ...without erasing the pattern. The old additive-toward-a-
		 * constant drove contrast to exactly 1.0 at full strength; a
		 * proportional swell keeps most of it.
		 *
		 * Headroom is tier-dependent and the bound reflects that
		 * honestly: crowd scenes are already bright, so the 1.0 clamp
		 * bites and costs more contrast there (measured 4.0x -> 2.6x at
		 * a quarter accent, vs 12.8x -> 9.2x for solo). The guarantee
		 * is that the pattern survives, not that it is untouched. */
		assert(contrast[1] > 0.6f * contrast[0]);
		/* At FULL accent the guarantee is only "no worse than the old
		 * behaviour". A dense scene on a small strip — the 7-LED jewel
		 * at crowd tier — starts bright enough that x3.2 puts every
		 * pixel past the 1.0 clamp, and clamping re-flattens it to
		 * 1.00x no matter how the boost is computed. That is a real
		 * limit of the headroom, not of the formula, and the frame
		 * power cap (led_budget.h) is what actually keeps those frames
		 * off the rail. The settings people use live at the bound
		 * above, which does hold everywhere. */
		assert(contrast[2] >= 1.f);
	}
}

static void test_ultra_mode_is_faster_and_blinks_in_unison(void)
{
	struct social_scene party_scene, ultra_scene;
	struct social_scene_plan party_plan, ultra_plan;

	social_scene_init(&party_scene, 0x5eedU, SOCIAL_CROWD, 5);
	social_scene_set_party(&party_scene, true);
	ultra_scene = party_scene;
	social_scene_set_ultra(&ultra_scene, true);
	social_scene_set_effect(&ultra_scene, 0.32f, 0.5f, false);

	social_scene_plan_at(&party_scene, 30.f, &party_plan);
	social_scene_plan_at(&ultra_scene, 30.f, &ultra_plan);
	assert(party_plan.party && !party_plan.ultra);
	assert(ultra_plan.party && ultra_plan.ultra);
	/* ultra compresses the already-fast party pacing further */
	assert(ultra_plan.period_s < party_plan.period_s);
	assert(ultra_plan.period_s >
	       party_plan.period_s * (SOCIAL_SCENE_ULTRA_SPEEDUP - 0.01f));
	/* identity is shared with party: same family, hues, saturation */
	assert(ultra_plan.family == party_plan.family);
	assert(ultra_plan.saturation == party_plan.saturation);
	assert(ultra_plan.effect_period_s > 0.31f);

	/* hard wave: shared-clock square, on for duty then off */
	assert(social_scene_effect_level(0.f, 0.32f, 0.5f, false) == 1.f);
	assert(social_scene_effect_level(0.32f * 0.75f, 0.32f, 0.5f, false) ==
	       0.f);
	assert(social_scene_effect_level(0.32f * 10.25f, 0.32f, 0.5f, false) ==
	       1.f);
	/* duty 0.7 moves the edge */
	assert(social_scene_effect_level(0.6f, 1.0f, 0.7f, false) == 1.f);
	assert(social_scene_effect_level(0.75f, 1.0f, 0.7f, false) == 0.f);
	/* no effect / solid duty are always full brightness */
	assert(social_scene_effect_level(0.75f, 0.f, 0.5f, false) == 1.f);
	assert(social_scene_effect_level(0.75f, 1.0f, 1.0f, false) == 1.f);
	assert(social_scene_effect_level(0.75f, 1.0f, 1.0f, true) == 1.f);

	/* during a hard off phase the rendered pixel is fully black */
	float off_t = 0.32f * 10.75f;
	float on_t = 0.32f * 10.25f;
	int lit_during_on = 0;

	for (int led = 0; led < TEST_LED_COUNT; led++) {
		struct frgb off_color = social_scene_render(
			&ultra_scene, off_t, led, TEST_LED_COUNT);

		assert(off_color.r == 0.f && off_color.g == 0.f &&
		       off_color.b == 0.f);
		struct frgb on_color = social_scene_render(
			&ultra_scene, on_t, led, TEST_LED_COUNT);

		if (on_color.r + on_color.g + on_color.b > 0.05f) {
			lit_during_on++;
		}
	}
	assert(lit_during_on > 0);
}

static void test_fade_wave_breathes_to_the_duty_floor(void)
{
	/* fade: raised cosine from 1.0 at phase 0 down to the duty floor at
	 * half period and back — never fully dark, always smooth */
	float top = social_scene_effect_level(0.f, 1.0f, 0.3f, true);
	float bottom = social_scene_effect_level(0.5f, 1.0f, 0.3f, true);
	float quarter = social_scene_effect_level(0.25f, 1.0f, 0.3f, true);

	assert(top > 0.99f);
	assert(bottom > 0.29f && bottom < 0.31f);
	assert(quarter > bottom && quarter < top);

	/* identical for every member at the same shared time */
	assert(social_scene_effect_level(7.25f, 1.0f, 0.3f, true) ==
	       social_scene_effect_level(7.25f, 1.0f, 0.3f, true));

	/* smoothness: no step exceeds a few percent at 50 Hz sampling */
	float previous = social_scene_effect_level(0.f, 1.0f, 0.2f, true);

	for (int i = 1; i <= 100; i++) {
		float level = social_scene_effect_level(
			(float)i * 0.02f, 1.0f, 0.2f, true);
		float step = level - previous;

		if (step < 0.f) {
			step = -step;
		}
		assert(step < 0.06f);
		previous = level;
	}

	/* a fading scene renders dimmed but never black at the trough */
	struct social_scene scene;

	social_scene_init(&scene, 0x5eedU, SOCIAL_CROWD, 5);
	social_scene_set_effect(&scene, 1.0f, 0.3f, true);
	int nonzero = 0;

	for (int led = 0; led < TEST_LED_COUNT; led++) {
		struct frgb c = social_scene_render(&scene, 0.5f, led,
						    TEST_LED_COUNT);

		if (c.r + c.g + c.b > 0.01f) {
			nonzero++;
		}
	}
	assert(nonzero > 0);
}

int main(void)
{
	test_plan_contract();
	test_motion_tiers_are_visibly_separated();
	test_crowd_palette_caps_at_five();
	test_rendering_is_bounded_repeatable_and_synchronized();
	test_minute_boundary_is_continuous();
	test_rendering_never_washes_to_pastel();
	test_composite_vivid_guard_handles_gray();
	test_crowd_remains_vivid_through_diffuser();
	test_pattern_is_different_every_adjacent_minute();
	test_party_mode_is_distinct_fast_vivid_and_bounded();
	test_party_flash_is_sparse_soft_synchronized_and_bright();
	test_party_accent_scales_and_can_be_disabled();
	test_party_accent_preserves_contrast();
	test_ultra_mode_is_faster_and_blinks_in_unison();
	test_fade_wave_breathes_to_the_duty_floor();
	puts("social scene tests passed");
	return 0;
}
