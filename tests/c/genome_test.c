#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "genome.h"

#ifndef TEST_LED_COUNT
#define TEST_LED_COUNT 16
#endif

static void assert_color(struct frgb color)
{
	assert(isfinite(color.r));
	assert(isfinite(color.g));
	assert(isfinite(color.b));
	assert(color.r >= 0.0f && color.r <= 1.0f);
	assert(color.g >= 0.0f && color.g <= 1.0f);
	assert(color.b >= 0.0f && color.b <= 1.0f);
}

static void assert_same_color(struct frgb first, struct frgb second)
{
	assert(first.r == second.r);
	assert(first.g == second.g);
	assert(first.b == second.b);
}

static void test_seed_expansion_is_deterministic(void)
{
	for (uint32_t seed = 0; seed < 1000; seed += 17) {
		struct genome first;
		struct genome second;

		genome_from_seed(seed, &first);
		genome_from_seed(seed, &second);
		assert(first.base == second.base);
		assert(first.hue == second.hue);
		assert(first.overlay == second.overlay);
		assert(first.ov_amt == second.ov_amt);
		assert(first.tempo == second.tempo);
		assert(first.phase_s == second.phase_s);
		assert(first.salt == second.salt);
		assert(first.base >= 4 && first.base <= 6);
		assert(first.overlay < 5);
		assert(first.ov_amt >= 0.25f && first.ov_amt <= 0.60f);
		assert(first.tempo >= 0.8f && first.tempo <= 1.2f);
		assert(first.phase_s >= 0.0f && first.phase_s <= 8.0f);
	}
}

static void test_one_minute_rotation(void)
{
	struct genome genome;

	genome_from_seed(0x12345678U, &genome);
	int at_zero = genome_base_at(&genome, 0.0f);
	int at_one = genome_base_at(&genome, 60.0f);
	int at_two = genome_base_at(&genome, 120.0f);

	assert(genome_base_at(&genome, 59.999f) == at_zero);
	assert(at_one != at_zero);
	assert(at_two != at_one && at_two != at_zero);
	assert(genome_base_at(&genome, 180.0f) == at_zero);
}

static void test_rendering_is_bounded_and_repeatable(void)
{
	const float times[] = { -0.1f, 0.0f, 2.99f, 3.0f, 59.999f,
				60.0f, 60.001f, 3599.5f };

	for (uint32_t seed = 0; seed < 64; seed++) {
		struct genome genome;

		genome_from_seed(seed, &genome);
		for (size_t t = 0; t < sizeof(times) / sizeof(times[0]); t++) {
			for (int led = 0; led < TEST_LED_COUNT; led++) {
				struct frgb first = genome_render(
					&genome, times[t], led, TEST_LED_COUNT);
				struct frgb second = genome_render(
					&genome, times[t], led, TEST_LED_COUNT);

				assert_color(first);
				assert_same_color(first, second);
			}
		}
	}

	for (int base = 0; base < GENOME_N_BASES; base++) {
		for (int led = 0; led < TEST_LED_COUNT; led++) {
			assert_color(genome_render_base(base, 17.25f, led,
						TEST_LED_COUNT));
		}
	}
}

static void test_rotation_boundary_is_continuous(void)
{
	struct genome genome;
	/* Keep the sampled spatial travel equivalent to the original 16-pixel
	 * test. A fixed time epsilon crosses more pixels on longer layouts even
	 * though the renderer is continuous. */
	const float epsilon = 0.001f * 16.f / (float)TEST_LED_COUNT;

	genome_from_seed(0x89abcdefU, &genome);
	for (int led = 0; led < TEST_LED_COUNT; led++) {
		struct frgb before = genome_render(&genome, 60.f - epsilon, led,
						   TEST_LED_COUNT);
		struct frgb after = genome_render(&genome, 60.f + epsilon, led,
						  TEST_LED_COUNT);
		float delta = fabsf(before.r - after.r) + fabsf(before.g - after.g) +
			      fabsf(before.b - after.b);

		assert(delta < 0.02f);
	}
}

int main(void)
{
	test_seed_expansion_is_deterministic();
	test_one_minute_rotation();
	test_rendering_is_bounded_and_repeatable();
	test_rotation_boundary_is_continuous();
	puts("genome tests passed");
	return 0;
}
