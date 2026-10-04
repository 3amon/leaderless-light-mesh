/* merge_trace — print a CSV of clock spread while two groups merge.
 *
 * Measurement mode, asserts nothing. It reuses the clock simulator in
 * clock_sim_test.c unchanged (six nodes, each running the firmware's real
 * clock_estimator.c, +/-300 ppm crystals, 2-8 ms transit with occasional
 * 30 ms excursions) and replays that suite's split-halves scenario:
 *
 *   t = -90 s   two halves of three nodes, no radio between them; one half
 *               starts 800 ms away from the other
 *   t =   0 s   the halves come into range
 *   t = +120 s  end
 *
 * Columns (milliseconds unless noted):
 *   t_s          seconds relative to the merge
 *   phase        "split" or "merged"
 *   spread_all   worst pairwise disagreement across all six clocks
 *   spread_a     worst pair inside half A (nodes 0-2)
 *   spread_b     worst pair inside half B (nodes 3-5)
 *   gap_ab       mean(A) - mean(B)
 *   n0..n5       each node's offset from the six-node mean
 *
 * Build and run from the repository root:
 *
 *   cc -std=c11 -O2 -Ifirmware/thread-ring/src -Itests/c \
 *      firmware/thread-ring/src/clock_estimator.c tests/c/merge_trace.c \
 *      -lm -o /tmp/merge_trace && /tmp/merge_trace > merge-convergence.csv
 *
 * This file was added when the repository was prepared for publication; it
 * is not part of the firmware or of the asserted host suite.
 */

#define main clock_sim_suite_main
#include "clock_sim_test.c"
#undef main

#define SPLIT_S 90
#define MERGED_S 120
#define SAMPLE_MS 200

static double worst_pair_us(int lo, int hi, int64_t truth_us)
{
	double worst = 0.0;

	for (int a = lo; a < hi; a++) {
		for (int b = a + 1; b < hi; b++) {
			double gap = fabs(global_of(&node[a], truth_us) -
					  global_of(&node[b], truth_us));

			if (gap > worst) {
				worst = gap;
			}
		}
	}
	return worst;
}

static double mean_us(int lo, int hi, int64_t truth_us)
{
	double sum = 0.0;

	for (int i = lo; i < hi; i++) {
		sum += global_of(&node[i], truth_us);
	}
	return sum / (double)(hi - lo);
}

static void emit(void)
{
	int64_t truth_us = now_ms * 1000;
	double all = mean_us(0, NODES, truth_us);

	printf("%.1f,%s,%.3f,%.3f,%.3f,%.3f",
	       (double)(now_ms - SPLIT_S * 1000) / 1000.0,
	       split_halves ? "split" : "merged",
	       worst_pair_us(0, NODES, truth_us) / 1000.0,
	       worst_pair_us(0, NODES / 2, truth_us) / 1000.0,
	       worst_pair_us(NODES / 2, NODES, truth_us) / 1000.0,
	       (mean_us(0, NODES / 2, truth_us) -
		mean_us(NODES / 2, NODES, truth_us)) / 1000.0);
	for (int i = 0; i < NODES; i++) {
		printf(",%.3f", (global_of(&node[i], truth_us) - all) / 1000.0);
	}
	printf("\n");
}

int main(void)
{
	/* Same seed and setup as
	 * test_split_halves_merge_despite_outlier_rejection(). */
	sim_reset(0x5eed0004U, false);
	split_halves = true;
	for (int i = 0; i < NODES / 2; i++) {
		clock_estimator_scramble(&node[i].estimator, 800000.0);
	}

	printf("t_s,phase,spread_all,spread_a,spread_b,gap_ab,n0,n1,n2,n3,n4,n5\n");
	for (int ms = 0; ms < (SPLIT_S + MERGED_S) * 1000; ms++) {
		if (ms == SPLIT_S * 1000) {
			split_halves = false;
		}
		if (ms % SAMPLE_MS == 0) {
			emit();
		}
		sim_step_ms();
	}
	emit();
	(void)clock_sim_suite_main;
	return 0;
}
