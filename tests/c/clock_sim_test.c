/* clock_sim — deterministic multi-node simulation of the REAL clock
 * estimator (clock_estimator.c), exchanging beacons over a jittery radio
 * with per-node crystal error. Every node's disciplined clock is the
 * actual production estimator; the harness only fabricates transit and
 * crystals.
 *
 * The rogue scenario is the evidence for the median hardening: a member
 * broadcasting a constant lie must not drag the honest crowd's shared
 * clock away from where it converged. TIME-SYNC.md called for
 * median-not-mean from the start; this simulator is where that claim is
 * finally enforced.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "clock_estimator.h"

#define NODES 6
#define BEACON_MS 2000
#define UPDATE_MS 400

struct clock_node {
	double ppm;              /* crystal error */
	int64_t offset_us;       /* initial local-clock offset */
	bool rogue;              /* broadcasts truth + lie_us, never listens */
	int64_t lie_us;
	struct clock_estimator estimator;
	struct clock_peer_estimator peer[NODES];
	int64_t first_heard_ms[NODES];   /* trust ramps with contact age */
	int64_t next_beacon_ms;
	int64_t next_update_ms;
};

static struct clock_node node[NODES];
static int64_t now_ms;
static uint32_t rng_state;
static bool split_halves;   /* no radio between halves while true */

static uint32_t rng(void)
{
	rng_state = rng_state * 1664525U + 1013904223U;
	return rng_state;
}

static int64_t local_us_of(const struct clock_node *n, int64_t truth_us)
{
	return truth_us + n->offset_us +
	       (int64_t)((double)truth_us * n->ppm * 1e-6);
}

static double global_of(const struct clock_node *n, int64_t truth_us)
{
	return clock_estimator_global_at(&n->estimator,
					 local_us_of(n, truth_us));
}

static void sim_reset(uint32_t seed, bool with_rogue)
{
	memset(node, 0, sizeof(node));
	now_ms = 0;
	rng_state = seed;
	split_halves = false;
	for (int i = 0; i < NODES; i++) {
		struct clock_node *n = &node[i];

		n->ppm = -300.0 + (double)(rng() % 600U);
		n->offset_us = -500000 + (int64_t)(rng() % 1000000U);
		clock_estimator_init(&n->estimator, local_us_of(n, 0));
		for (int p = 0; p < NODES; p++) {
			clock_peer_estimator_init(&n->peer[p]);
			n->first_heard_ms[p] = -1;
		}
		n->next_beacon_ms = (int64_t)(rng() % BEACON_MS);
		n->next_update_ms = (int64_t)(rng() % UPDATE_MS);
	}
	if (with_rogue) {
		node[NODES - 1].rogue = true;
		node[NODES - 1].lie_us = 10000000;   /* ten seconds of lie */
	}
}

static void sim_step_ms(void)
{
	now_ms++;
	int64_t truth_us = now_ms * 1000;

	for (int tx = 0; tx < NODES; tx++) {
		struct clock_node *sender = &node[tx];

		if (now_ms < sender->next_beacon_ms) {
			continue;
		}
		sender->next_beacon_ms =
			now_ms + BEACON_MS / 2 + (int64_t)(rng() % BEACON_MS);

		int64_t sender_local = local_us_of(sender, truth_us);
		double sender_global = sender->rogue
			? (double)truth_us + (double)sender->lie_us
			: clock_estimator_global_at(&sender->estimator,
						    sender_local);

		for (int rx = 0; rx < NODES; rx++) {
			struct clock_node *receiver = &node[rx];

			if (rx == tx || receiver->rogue) {
				continue;
			}
			if (split_halves &&
			    (tx < NODES / 2) != (rx < NODES / 2)) {
				continue;
			}
			/* 2-8 ms transit, occasional 30 ms MPL excursion */
			int64_t transit = 2000 + (int64_t)(rng() % 6000U);

			if (rng() % 20U == 0U) {
				transit += 30000;
			}
			int64_t arrival_local =
				local_us_of(receiver, truth_us + transit);

			if (receiver->first_heard_ms[tx] < 0) {
				receiver->first_heard_ms[tx] = now_ms;
			}
			clock_peer_estimator_add_sample(
				&receiver->peer[tx],
				&clock_estimator_default_config,
				arrival_local, (int64_t)sender_global,
				sender_local, sender->estimator.h);
		}
	}

	for (int i = 0; i < NODES; i++) {
		struct clock_node *n = &node[i];

		if (n->rogue || now_ms < n->next_update_ms) {
			continue;
		}
		n->next_update_ms = now_ms + UPDATE_MS;

		struct clock_peer_input inputs[NODES];
		size_t count = 0;

		for (int p = 0; p < NODES; p++) {
			if (p == i) {
				continue;
			}
			/* mirror the production trust ramp: stranger 0.05,
			 * candidate climbing to 0.6 over the 20 s join run,
			 * then full member weight */
			float weight = 0.05f;

			if (n->first_heard_ms[p] >= 0) {
				float age_s = (float)(now_ms -
						      n->first_heard_ms[p]) /
					      1000.f;

				weight = age_s >= 20.f
					? 1.0f
					: 0.1f + 0.025f * age_s;
			}
			clock_peer_estimator_set_weight(&n->peer[p], weight);
			inputs[count++] = (struct clock_peer_input){
				.peer = &n->peer[p],
				.fresh = n->peer[p].phase_count > 0,
			};
		}
		clock_estimator_update(&n->estimator,
				       &clock_estimator_default_config, inputs,
				       count, local_us_of(n, truth_us),
				       UPDATE_MS * 1000);
	}
}

static void sim_run_s(int seconds)
{
	for (int s = 0; s < seconds * 1000; s++) {
		sim_step_ms();
	}
}

static double honest_worst_pair_us(void)
{
	int64_t truth_us = now_ms * 1000;
	double worst = 0.0;

	for (int a = 0; a < NODES; a++) {
		for (int b = a + 1; b < NODES; b++) {
			if (node[a].rogue || node[b].rogue) {
				continue;
			}
			double gap = fabs(global_of(&node[a], truth_us) -
					  global_of(&node[b], truth_us));

			if (gap > worst) {
				worst = gap;
			}
		}
	}
	return worst;
}

static double honest_crowd_mean_global(void)
{
	int64_t truth_us = now_ms * 1000;
	double sum = 0.0;
	int count = 0;

	for (int i = 0; i < NODES; i++) {
		if (!node[i].rogue) {
			sum += global_of(&node[i], truth_us);
			count++;
		}
	}
	return sum / (double)count;
}

static void test_crowd_converges_and_holds(void)
{
	sim_reset(0x5eed0001U, false);
	sim_run_s(90);
	assert(honest_worst_pair_us() < 15000.0);

	/* five steady minutes: the lock must never degrade */
	for (int minute = 0; minute < 5; minute++) {
		sim_run_s(60);
		assert(honest_worst_pair_us() < 15000.0);
	}
}

static void test_scramble_heals(void)
{
	sim_reset(0x5eed0002U, false);
	sim_run_s(90);
	assert(honest_worst_pair_us() < 15000.0);

	clock_estimator_scramble(&node[2].estimator, 500000.0);
	sim_run_s(30);
	assert(honest_worst_pair_us() < 15000.0);
}

/* THE median-not-mean evidence: a member with full trust broadcasting a
 * constant ten-second lie. The honest crowd must stay mutually locked AND
 * anchored — its shared clock may not slide toward the liar. Under the
 * old weighted-mean phase step the crowd chased the rogue at an unbounded
 * rate (hundreds of milliseconds within the first minutes); the weighted
 * median ignores a single liar outright in any crowd of three or more. */
static void test_rogue_lie_cannot_capture_the_crowd(void)
{
	/* Under the old weighted-mean phase step the crowd did not merely
	 * drift: from cold start it CONVERGED ONTO the rogue's clock
	 * outright (measured crowd offset ~9.97 s of the 10 s lie, all
	 * honest nodes mutually locked to the liar). With the weighted
	 * median, snap quorum, and minority-outlier rejection the liar is
	 * simply outvoted.
	 *
	 * A leaderless clock has NO truth reference, so the honest
	 * consensus wanders slowly and collectively (transit bias, a few
	 * ms/s, invisible to eyes and inherent to the design). The
	 * assertions therefore check what actually matters: the crowd
	 * stays mutually locked, lands in its own honest region rather
	 * than the liar's, keeps measuring the liar as ~10 s away, and
	 * never closes on the lie. */
	sim_reset(0x5eed0003U, true);
	sim_run_s(90);
	double lie = 10000000.0;
	double to_lie_at_90 = lie - (honest_crowd_mean_global() -
				     (double)(now_ms * 1000));

	assert(honest_worst_pair_us() < 15000.0);
	assert(to_lie_at_90 > 8000000.0);

	sim_run_s(300);
	double to_lie_at_end = lie - (honest_crowd_mean_global() -
				      (double)(now_ms * 1000));

	assert(honest_worst_pair_us() < 15000.0);
	assert(to_lie_at_end > 8000000.0);
	/* not being pulled toward the lie (the inherent wander happens to
	 * run the other way; equality margin covers noise) */
	assert(to_lie_at_end > to_lie_at_90 - 100000.0);
	/* honest nodes still measure the liar ~10 s away: uncaptured */
	assert(node[0].peer[NODES - 1].last_delta_us > 8000000);
}

/* Two long-split halves whose clocks diverged past the outlier-rejection
 * bound must still MERGE on reunion: rejection only ever discards a
 * minority of the vote weight, so a 50/50 encounter falls back to the
 * plain weighted median and one side yields. This is the scenario naive
 * outlier rejection would turn into a permanent schism. */
static void test_split_halves_merge_despite_outlier_rejection(void)
{
	/* The offset must be baked in BEFORE any samples exist: scrambling
	 * a converged half self-heals within seconds through each node's
	 * own pre-scramble sample window (the same dynamics as the
	 * hardware scramble-500 drills), which is itself a nice property
	 * this harness demonstrated. */
	sim_reset(0x5eed0004U, false);
	split_halves = true;
	for (int i = 0; i < NODES / 2; i++) {
		clock_estimator_scramble(&node[i].estimator, 800000.0);
	}
	sim_run_s(90);   /* halves converge internally, far apart */
	assert(honest_worst_pair_us() > 300000.0);

	split_halves = false;
	sim_run_s(90);
	assert(honest_worst_pair_us() < 15000.0);
}

/* Regression for the v0.1.75 field incident: the rate median's even-count
 * pick landed on the UPPER-middle, a half-spread-high bias that compounded
 * through their_h every update — the live fleet's trim ratcheted
 * +300 ppm/min toward the clamp while phase stayed perfectly locked (the
 * failure was invisible to eyes). The crowd's collective trim must stay
 * bounded near the crystals' true spread over a long run. */
static void test_rate_trim_stays_bounded(void)
{
	sim_reset(0x5eed0005U, false);
	sim_run_s(720);
	for (int i = 0; i < NODES; i++) {
		double trim_ppm = (node[i].estimator.h - 1.0) * 1e6;

		assert(trim_ppm > -1000.0 && trim_ppm < 1000.0);
	}
	assert(honest_worst_pair_us() < 15000.0);
}

int main(void)
{
	test_crowd_converges_and_holds();
	test_scramble_heals();
	test_rogue_lie_cannot_capture_the_crowd();
	test_split_halves_merge_despite_outlier_rejection();
	test_rate_trim_stays_bounded();
	printf("clock estimator simulation passed\n");
	return 0;
}
