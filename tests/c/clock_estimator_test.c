#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "clock_estimator.h"

static void add_sample(struct clock_peer_estimator *peer, int64_t local_us,
		       int64_t global_us, int64_t raw_us)
{
	clock_peer_estimator_add_sample(
		peer, &clock_estimator_default_config, local_us, global_us, raw_us,
		1.0);
}

static int64_t error_at(const struct clock_estimator *estimator,
			int64_t local_us, int64_t reference_us)
{
	return (int64_t)clock_estimator_global_at(estimator, local_us) -
	       reference_us;
}

static void test_clean_convergence_trace(void)
{
	struct clock_estimator estimator;
	struct clock_peer_estimator peer;
	struct clock_peer_input input = {.peer = &peer, .fresh = true};

	clock_estimator_init(&estimator, 0);
	clock_peer_estimator_init(&peer);
	clock_estimator_scramble(&estimator, 200000);

	for (int64_t now = 500000; now <= 30000000; now += 500000) {
		add_sample(&peer, now, now, now);
		clock_estimator_update(&estimator, &clock_estimator_default_config,
				       &input, 1, now, 500000);
	}
	assert(llabs(error_at(&estimator, 30000000, 30000000)) < 1500);
	assert(peer.locked);
}

static void test_loss_and_recovery_trace(void)
{
	struct clock_estimator estimator;
	struct clock_peer_estimator peer;
	struct clock_peer_input input = {.peer = &peer, .fresh = true};

	clock_estimator_init(&estimator, 0);
	clock_peer_estimator_init(&peer);
	estimator.h = 0.997;

	for (int64_t now = 500000; now <= 2000000; now += 500000) {
		add_sample(&peer, now, now, now);
		clock_estimator_update(&estimator, &clock_estimator_default_config,
				       &input, 1, now, 500000);
	}

	/* Short packet loss retains the fresh input and recent phase window. */
	for (int64_t now = 2500000; now <= 4000000; now += 500000) {
		clock_estimator_update(&estimator, &clock_estimator_default_config,
				       &input, 1, now, 500000);
	}
	double before_partition_h = estimator.h;
	double before_update = clock_estimator_global_at(&estimator, 4500000);

	input.fresh = false;
	clock_estimator_update(&estimator, &clock_estimator_default_config, &input,
			       1, 4500000, 500000);
	assert(clock_estimator_global_at(&estimator, 4500000) == before_update);
	assert(fabs(1.0 - estimator.h) < fabs(1.0 - before_partition_h));

	for (int64_t now = 5000000; now <= 15000000; now += 500000) {
		clock_estimator_update(&estimator, &clock_estimator_default_config,
				       &input, 1, now, 500000);
	}
	assert(fabs(1.0 - estimator.h) < 0.002);

	input.fresh = true;
	for (int64_t now = 15500000; now <= 30000000; now += 500000) {
		add_sample(&peer, now, now, now);
		clock_estimator_update(&estimator, &clock_estimator_default_config,
				       &input, 1, now, 500000);
	}
	assert(llabs(error_at(&estimator, 30000000, 30000000)) < 6000);
}

static void test_phase_step_trace(void)
{
	struct clock_estimator estimator;
	struct clock_peer_estimator peer;
	struct clock_peer_input input = {.peer = &peer, .fresh = true};

	clock_estimator_init(&estimator, 0);
	clock_peer_estimator_init(&peer);
	for (int64_t now = 500000; now <= 1500000; now += 500000) {
		add_sample(&peer, now, now, now);
		clock_estimator_update(&estimator, &clock_estimator_default_config,
				       &input, 1, now, 500000);
	}
	assert(peer.phase_count == 3);

	add_sample(&peer, 2000000, 2100000, 2000000);
	assert(peer.phase_count == 1); /* Old phase stamps became invalid. */
	clock_estimator_update(&estimator, &clock_estimator_default_config,
			       &input, 1, 2000000, 500000);
	assert(error_at(&estimator, 2000000, 2000000) == 50000);

	for (int64_t now = 2500000; now <= 15000000; now += 500000) {
		add_sample(&peer, now, now + 100000, now);
		clock_estimator_update(&estimator, &clock_estimator_default_config,
				       &input, 1, now, 500000);
	}
	assert(llabs(error_at(&estimator, 15000000, 15100000)) < 1500);
}

static void test_skew_trace(void)
{
	struct clock_estimator estimator;
	struct clock_peer_estimator peer;
	struct clock_peer_input input = {.peer = &peer, .fresh = true};

	clock_estimator_init(&estimator, 0);
	clock_peer_estimator_init(&peer);

	int64_t local_now = 0;
	for (int64_t reference = 500000; reference <= 120000000;
	     reference += 500000) {
		local_now = reference + reference * 300 / 1000000;
		add_sample(&peer, local_now, reference, reference);
		clock_estimator_update(&estimator, &clock_estimator_default_config,
				       &input, 1, local_now, 500150);
	}
	double rate_ppm = (estimator.h - 1.0) * 1000000.0;

	assert(rate_ppm < -280.0 && rate_ppm > -320.0);
	assert(llabs(error_at(&estimator, local_now, 120000000)) < 6000);
}

static void test_partition_trace(void)
{
	struct clock_estimator estimator;
	struct clock_peer_estimator lower;
	struct clock_peer_estimator upper;
	struct clock_peer_input inputs[2] = {
		{.peer = &lower, .fresh = true},
		{.peer = &upper, .fresh = true},
	};

	clock_estimator_init(&estimator, 0);
	clock_peer_estimator_init(&lower);
	clock_peer_estimator_init(&upper);

	for (int64_t now = 500000; now <= 10000000; now += 500000) {
		add_sample(&lower, now, now - 30000, now);
		add_sample(&upper, now, now + 30000, now);
		clock_estimator_update(&estimator, &clock_estimator_default_config,
				       inputs, 2, now, 500000);
	}
	assert(llabs(error_at(&estimator, 10000000, 10000000)) < 1500);

	/* Partition onto the upper clock domain, then merge both views again. */
	inputs[0].fresh = false;
	for (int64_t now = 10500000; now <= 25000000; now += 500000) {
		add_sample(&upper, now, now + 30000, now);
		clock_estimator_update(&estimator, &clock_estimator_default_config,
				       inputs, 2, now, 500000);
	}
	assert(llabs(error_at(&estimator, 25000000, 25030000)) < 1500);

	inputs[0].fresh = true;
	for (int64_t now = 25500000; now <= 45000000; now += 500000) {
		add_sample(&lower, now, now - 30000, now);
		add_sample(&upper, now, now + 30000, now);
		clock_estimator_update(&estimator, &clock_estimator_default_config,
				       inputs, 2, now, 500000);
	}
	assert(llabs(error_at(&estimator, 45000000, 45000000)) < 1500);
}

int main(void)
{
	test_clean_convergence_trace();
	test_loss_and_recovery_trace();
	test_phase_step_trace();
	test_skew_trace();
	test_partition_trace();
	puts("clock estimator tests passed");
	return 0;
}
