/* Pure leaderless clock-estimator policy; no Zephyr or socket dependencies. */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLOCK_PHASE_SAMPLES 10
#define CLOCK_RAW_SAMPLES 40
/* Consensus vote capacity per update; matches the sync layer's
 * SYNC_MAX_PEERS (asserted where both headers meet in sync.c). */
#define CLOCK_ESTIMATOR_MAX_PEERS 23

struct clock_phase_sample {
	int64_t local_arrival_us;
	int64_t peer_global_us;
};

struct clock_raw_sample {
	int64_t local_arrival_us;
	int64_t peer_raw_us;
};

struct clock_peer_estimator {
	double their_h;
	float weight;
	struct clock_phase_sample phase[CLOCK_PHASE_SAMPLES];
	int phase_head;
	int phase_count;
	struct clock_raw_sample raw[CLOCK_RAW_SAMPLES];
	int raw_head;
	int raw_count;
	int64_t last_delta_us;
	bool locked;
};

struct clock_estimator {
	double h;
	double theta_us;
	int64_t epoch_us;
};

struct clock_estimator_config {
	int64_t phase_window_us;
	int64_t raw_window_us;
	double step_threshold_us;
	double step_drift_allowance;
	double window_disagreement_us;
	double snap_us;
	double deadband_us;
	double slew_us_per_s;
	double quiet_residual_us;
	double h_min;
	double h_max;
	int64_t lock_gate_us;
	int minimum_rate_samples;
	int64_t minimum_rate_span_us;
	double rate_gain;
	double alone_relax_gain;
	/* Below this much total vote weight the phase step is slew-only: a
	 * cold-booting node must not SNAP onto its very first voice (which
	 * could be a liar) before the trust ramp has vouched for anyone.
	 * One full member's weight is the quorum. */
	double snap_quorum_weight;
	/* Votes farther than this from the preliminary median are dropped —
	 * but only when they hold a MINORITY of the vote weight. A lone liar
	 * therefore cannot even bias the consensus quantile, while a
	 * long-split subgroup reuniting with half the weight still merges
	 * instead of the two sides rejecting each other forever. */
	double outlier_reject_us;
};

extern const struct clock_estimator_config clock_estimator_default_config;

struct clock_peer_input {
	struct clock_peer_estimator *peer;
	bool fresh;
};

void clock_estimator_init(struct clock_estimator *estimator,
			  int64_t local_now_us);
double clock_estimator_global_at(const struct clock_estimator *estimator,
				 int64_t local_us);
void clock_estimator_scramble(struct clock_estimator *estimator,
			      double offset_us);

void clock_peer_estimator_init(struct clock_peer_estimator *peer);
void clock_peer_estimator_set_weight(struct clock_peer_estimator *peer,
				     float weight);
void clock_peer_estimator_add_sample(
	struct clock_peer_estimator *peer,
	const struct clock_estimator_config *config, int64_t local_arrival_us,
	int64_t peer_global_us, int64_t peer_raw_us, double peer_h);

void clock_estimator_update(struct clock_estimator *estimator,
			    const struct clock_estimator_config *config,
			    struct clock_peer_input *inputs, size_t input_count,
			    int64_t local_now_us, int64_t dt_us);
