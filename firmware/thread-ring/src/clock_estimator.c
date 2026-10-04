#include "clock_estimator.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

const struct clock_estimator_config clock_estimator_default_config = {
	.phase_window_us = 2600000LL,
	.raw_window_us = 20000000LL,
	.step_threshold_us = 40000.0,
	.step_drift_allowance = 0.01,
	.window_disagreement_us = 25000.0,
	.snap_us = 80000.0,
	.deadband_us = 1500.0,
	.slew_us_per_s = 60000.0,
	.quiet_residual_us = 6000.0,
	.h_min = 0.994,
	.h_max = 1.006,
	.lock_gate_us = 6000,
	.minimum_rate_samples = 8,
	.minimum_rate_span_us = 8000000LL,
	.rate_gain = 0.1,
	.alone_relax_gain = 0.02,
	.snap_quorum_weight = 1.0,
	.outlier_reject_us = 250000.0,
};

void clock_estimator_init(struct clock_estimator *estimator,
			  int64_t local_now_us)
{
	memset(estimator, 0, sizeof(*estimator));
	estimator->h = 1.0;
	estimator->epoch_us = local_now_us;
}

double clock_estimator_global_at(const struct clock_estimator *estimator,
				 int64_t local_us)
{
	return estimator->h * (double)(local_us - estimator->epoch_us) +
	       estimator->theta_us;
}

void clock_estimator_scramble(struct clock_estimator *estimator,
			      double offset_us)
{
	estimator->theta_us += offset_us;
}

void clock_peer_estimator_init(struct clock_peer_estimator *peer)
{
	memset(peer, 0, sizeof(*peer));
	peer->their_h = 1.0;
	peer->weight = 1.0f;
}

void clock_peer_estimator_set_weight(struct clock_peer_estimator *peer,
				     float weight)
{
	peer->weight = weight;
}

void clock_peer_estimator_add_sample(
	struct clock_peer_estimator *peer,
	const struct clock_estimator_config *config, int64_t local_arrival_us,
	int64_t peer_global_us, int64_t peer_raw_us, double peer_h)
{
	peer->their_h = peer_h;
	peer->raw[peer->raw_head] = (struct clock_raw_sample){
		.local_arrival_us = local_arrival_us,
		.peer_raw_us = peer_raw_us,
	};
	peer->raw_head = (peer->raw_head + 1) % CLOCK_RAW_SAMPLES;
	if (peer->raw_count < CLOCK_RAW_SAMPLES) {
		peer->raw_count++;
	}

	if (peer->phase_count > 0) {
		int last = (peer->phase_head - 1 + CLOCK_PHASE_SAMPLES) %
			   CLOCK_PHASE_SAMPLES;
		struct clock_phase_sample *previous = &peer->phase[last];
		double current_offset =
			(double)(peer_global_us - local_arrival_us);
		double previous_offset =
			(double)(previous->peer_global_us -
				 previous->local_arrival_us);
		double allowance = config->step_threshold_us +
			config->step_drift_allowance *
			(double)(local_arrival_us - previous->local_arrival_us);

		if (fabs(current_offset - previous_offset) > allowance) {
			peer->phase_count = 0;
			peer->phase_head = 0;
		}
	}

	peer->phase[peer->phase_head] = (struct clock_phase_sample){
		.local_arrival_us = local_arrival_us,
		.peer_global_us = peer_global_us,
	};
	peer->phase_head = (peer->phase_head + 1) % CLOCK_PHASE_SAMPLES;
	if (peer->phase_count < CLOCK_PHASE_SAMPLES) {
		peer->phase_count++;
	}
}

/* One (delta, weight) vote per fresh peer. The consensus statistic is the
 * WEIGHTED MEDIAN, not the mean: TIME-SYNC.md flagged median-not-mean as a
 * required hardening, and the clock simulator proved why — under the mean,
 * a single fully-trusted member broadcasting a constant ten-second lie
 * didn't just drag the crowd, the crowd converged onto the lie outright.
 * The median outvotes any single liar in a crowd of three or more. */
struct clock_phase_vote {
	double delta;
	double weight;
};

static double weighted_median(struct clock_phase_vote *votes, size_t count,
			      double total_weight)
{
	for (size_t a = 1; a < count; a++) {
		for (size_t b = a;
		     b > 0 && votes[b].delta < votes[b - 1].delta; b--) {
			struct clock_phase_vote swap = votes[b];

			votes[b] = votes[b - 1];
			votes[b - 1] = swap;
		}
	}

	double accumulated = 0.0;

	for (size_t index = 0; index < count; index++) {
		accumulated += votes[index].weight;
		if (accumulated * 2.0 == total_weight && index + 1 < count) {
			/* The half-weight point lands exactly on the boundary
			 * between this vote and the next: take their
			 * midpoint, so an even symmetric pair of peers
			 * centers instead of favoring the lower vote. */
			return (votes[index].delta +
				votes[index + 1].delta) / 2.0;
		}
		if (accumulated * 2.0 >= total_weight) {
			return votes[index].delta;
		}
	}
	return votes[count - 1].delta;
}

static void update_phase(struct clock_estimator *estimator,
			 const struct clock_estimator_config *config,
			 struct clock_peer_input *inputs, size_t input_count,
			 int64_t local_now_us, int64_t dt_us)
{
	struct clock_phase_vote votes[CLOCK_ESTIMATOR_MAX_PEERS];
	size_t vote_count = 0;
	double total_weight = 0.0;

	for (size_t input = 0; input < input_count; input++) {
		struct clock_peer_estimator *peer = inputs[input].peer;

		if (!inputs[input].fresh || peer == NULL) {
			continue;
		}

		double high = -1e18;
		double low = 1e18;
		double newest_delta = 0.0;
		int64_t newest_arrival = -1;
		int recent = 0;

		for (int sample = 0; sample < peer->phase_count; sample++) {
			struct clock_phase_sample *phase = &peer->phase[sample];

			if (local_now_us - phase->local_arrival_us >
			    config->phase_window_us) {
				continue;
			}
			double delta = (double)phase->peer_global_us -
				clock_estimator_global_at(
					estimator, phase->local_arrival_us);

			if (delta > high) {
				high = delta;
			}
			if (delta < low) {
				low = delta;
			}
			if (phase->local_arrival_us > newest_arrival) {
				newest_arrival = phase->local_arrival_us;
				newest_delta = delta;
			}
			recent++;
		}

		if (recent == 0) {
			continue;
		}
		double delta = high - low > config->window_disagreement_us
				       ? newest_delta
				       : high;

		peer->last_delta_us = (int64_t)delta;
		peer->locked = llabs((long long)peer->last_delta_us) <
			       config->lock_gate_us;
		double weight = peer->weight > 0.0f ? (double)peer->weight : 1e-3;

		if (vote_count < CLOCK_ESTIMATOR_MAX_PEERS) {
			votes[vote_count].delta = delta;
			votes[vote_count].weight = weight;
			vote_count++;
			total_weight += weight;
		}
	}

	if (vote_count == 0 || total_weight == 0.0) {
		return;
	}

	double consensus = weighted_median(votes, vote_count, total_weight);

	/* Minority-outlier rejection around the preliminary median. */
	double near_weight = 0.0;

	for (size_t index = 0; index < vote_count; index++) {
		if (fabs(votes[index].delta - consensus) <=
		    config->outlier_reject_us) {
			near_weight += votes[index].weight;
		}
	}
	if (near_weight < total_weight && near_weight * 2.0 > total_weight) {
		struct clock_phase_vote near[CLOCK_ESTIMATOR_MAX_PEERS];
		size_t near_count = 0;

		for (size_t index = 0; index < vote_count; index++) {
			if (fabs(votes[index].delta - consensus) <=
			    config->outlier_reject_us) {
				near[near_count++] = votes[index];
			}
		}
		consensus = weighted_median(near, near_count, near_weight);
	}
	double slew_max = config->slew_us_per_s * (double)dt_us / 1000000.0;
	bool snap_allowed = total_weight >= config->snap_quorum_weight;

	if (snap_allowed && fabs(consensus) > config->snap_us) {
		estimator->theta_us += consensus * 0.5;
	} else if (fabs(consensus) > config->deadband_us) {
		double step = consensus * 0.5;

		if (step > slew_max) {
			step = slew_max;
		}
		if (step < -slew_max) {
			step = -slew_max;
		}
		estimator->theta_us += step;
	}
}

static void update_rate(struct clock_estimator *estimator,
			const struct clock_estimator_config *config,
			struct clock_peer_input *inputs, size_t input_count,
			int64_t local_now_us)
{
	double rates[CLOCK_ESTIMATOR_MAX_PEERS];
	int rate_count = 0;

	for (size_t input = 0; input < input_count; input++) {
		struct clock_peer_estimator *peer = inputs[input].peer;

		if (!inputs[input].fresh || peer == NULL ||
		    peer->raw_count < config->minimum_rate_samples) {
			continue;
		}

		double local_mean = 0.0;
		double peer_mean = 0.0;
		int sample_count = 0;
		int64_t local_min = INT64_MAX;
		int64_t local_max = INT64_MIN;

		for (int sample = 0; sample < peer->raw_count; sample++) {
			struct clock_raw_sample *raw = &peer->raw[sample];

			if (local_now_us - raw->local_arrival_us >
			    config->raw_window_us) {
				continue;
			}
			local_mean += (double)raw->local_arrival_us;
			peer_mean += (double)raw->peer_raw_us;
			if (raw->local_arrival_us < local_min) {
				local_min = raw->local_arrival_us;
			}
			if (raw->local_arrival_us > local_max) {
				local_max = raw->local_arrival_us;
			}
			sample_count++;
		}

		if (sample_count < config->minimum_rate_samples ||
		    local_max - local_min < config->minimum_rate_span_us) {
			continue;
		}
		local_mean /= (double)sample_count;
		peer_mean /= (double)sample_count;

		double local_variance = 0.0;
		double covariance = 0.0;

		for (int sample = 0; sample < peer->raw_count; sample++) {
			struct clock_raw_sample *raw = &peer->raw[sample];

			if (local_now_us - raw->local_arrival_us >
			    config->raw_window_us) {
				continue;
			}
			double local_delta =
				(double)raw->local_arrival_us - local_mean;

			local_variance += local_delta * local_delta;
			covariance += local_delta *
				      ((double)raw->peer_raw_us - peer_mean);
		}
		if (local_variance <= 0.0) {
			continue;
		}

		double slope = covariance / local_variance;
		double residual_sum = 0.0;

		for (int sample = 0; sample < peer->raw_count; sample++) {
			struct clock_raw_sample *raw = &peer->raw[sample];

			if (local_now_us - raw->local_arrival_us >
			    config->raw_window_us) {
				continue;
			}
			double residual = ((double)raw->peer_raw_us - peer_mean) -
				slope *
					((double)raw->local_arrival_us - local_mean);

			residual_sum += residual * residual;
		}
		if (sqrt(residual_sum / (double)sample_count) <
		    config->quiet_residual_us) {
			double rate = slope * peer->their_h;

			if (rate < config->h_min) {
				rate = config->h_min;
			}
			if (rate > config->h_max) {
				rate = config->h_max;
			}
			if (rate_count < CLOCK_ESTIMATOR_MAX_PEERS) {
				rates[rate_count++] = rate;
			}
		}
	}

	if (rate_count > 0) {
		/* median of rates: same one-liar immunity as the phase step,
		 * though the h_min/h_max clamp already bounded this axis */
		for (int a = 1; a < rate_count; a++) {
			for (int b = a; b > 0 && rates[b] < rates[b - 1]; b--) {
				double swap = rates[b];

				rates[b] = rates[b - 1];
				rates[b - 1] = swap;
			}
		}
		double rate_median = (rate_count % 2) != 0
			? rates[rate_count / 2]
			: (rates[rate_count / 2 - 1] +
			   rates[rate_count / 2]) / 2.0;

		estimator->h += (rate_median - estimator->h) *
				config->rate_gain;
	}
	if (estimator->h < config->h_min) {
		estimator->h = config->h_min;
	}
	if (estimator->h > config->h_max) {
		estimator->h = config->h_max;
	}
}

void clock_estimator_update(struct clock_estimator *estimator,
			    const struct clock_estimator_config *config,
			    struct clock_peer_input *inputs, size_t input_count,
			    int64_t local_now_us, int64_t dt_us)
{
	estimator->theta_us =
		clock_estimator_global_at(estimator, local_now_us);
	estimator->epoch_us = local_now_us;

	bool any_fresh = false;

	for (size_t input = 0; input < input_count; input++) {
		if (inputs[input].fresh && inputs[input].peer != NULL) {
			any_fresh = true;
			break;
		}
	}
	if (!any_fresh) {
		estimator->h +=
			(1.0 - estimator->h) * config->alone_relax_gain;
		return;
	}

	update_phase(estimator, config, inputs, input_count, local_now_us, dt_us);
	update_rate(estimator, config, inputs, input_count, local_now_us);
}
