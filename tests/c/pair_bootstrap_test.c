/* Two rings alone together, logical clocks hours apart: do they ever lock?
 *
 * Reproduces the 2026-09-29 field case (sleepy-mirage-wave up two days,
 * tender-jelly-ember freshly rebooted): each listed the other as a direct
 * member, each demoted the other for timing, neither ever snapped, and the
 * 19,000 s gap closed at the slew cap — about 58 hours. Runs the REAL
 * group engine coupled to the REAL clock estimator through the integrated
 * campaign harness, with asserts.
 *
 * The assertion is on the MECHANISM, not just the outcome: with the pair
 * rule the snap happens at membership, so the gap is gone within seconds
 * of both sides becoming members. Without it the simulator sometimes
 * still limps to a lock through the short window between agreement and
 * timing demotion (the field pair never did), but only tens of seconds
 * after membership — so "gap closed shortly after membership" separates
 * the two engines where "eventually lit" does not.
 *
 * Note the harness ticks the engine every 1.2-2 s, not the firmware's 1 s,
 * so its join takes ~40 s where the device takes ~20 s.
 */
#include <assert.h>
#include <math.h>

#define main integrated_main
#include "integrated.c"
#undef main

static double pair_gap_s(void)
{
	int64_t truth = now_ms * 1000;

	return (clock_estimator_global_at(&N[0].clk, local_us_of(&N[0], truth)) -
		clock_estimator_global_at(&N[1].clk, local_us_of(&N[1], truth))) /
	       1e6;
}

static bool pair_members(void)
{
	return group_engine_member_count(&N[0].engine) == 2 &&
	       group_engine_member_count(&N[1].engine) == 2;
}

/* Run until both sides hold each other as members (or give up), then
 * demand the gap is closed shortly after and the pair lights up. */
static void expect_pair_bootstrap(const char *label)
{
	int member_at = -1;

	for (int t = 1; t <= 120; t++) {
		irun_s(1);
		if (pair_members()) {
			member_at = t;
			break;
		}
	}
	if (member_at < 0) {
		fprintf(stderr, "%s: never mutual members in 120 s\n", label);
		assert(false);
	}
	irun_s(10);
	double gap = fabs(pair_gap_s());

	if (gap > 10.0) {
		fprintf(stderr,
			"%s: 10 s after mutual membership the gap is still %.1f s\n",
			label, gap);
		assert(false);
	}
	int lit_at = -1;

	for (int t = 0; t < 90; t++) {
		if (ilit(0, 1) == 2 && pair_members()) {
			lit_at = t;
			break;
		}
		irun_s(1);
	}
	if (lit_at < 0) {
		fprintf(stderr, "%s: gap closed but never lit within 90 s\n", label);
		assert(false);
	}
	/* and it stays locked: no yo-yo between the two snapping halves */
	int dark = 0;

	for (int t = 0; t < 120; t++) {
		irun_s(1);
		if (ilit(0, 1) < 2) {
			dark++;
		}
	}
	assert(dark == 0);
	assert(group_engine_tier(&N[0].engine) == SOCIAL_DUO);
}

static void test_far_apart_pair_locks(void)
{
	static const int64_t gaps_us[] = {
		19000LL * 1000000LL,   /* the field case: 5.3 hours */
		600LL * 1000000LL,     /* ten minutes */
		5LL * 1000000LL,       /* five seconds: boot skew of a paired start */
	};

	for (size_t g = 0; g < sizeof(gaps_us) / sizeof(gaps_us[0]); g++) {
		iinit(2, 0xA1B0 + (uint32_t)g);
		/* Logical time starts at zero at boot, so a gap is a difference
		 * in how long each node has been running: give node 1 a head
		 * start rather than a hardware-clock offset (which the
		 * estimator's boot epoch cancels). */
		N[1].clk.theta_us += (double)gaps_us[g];
		expect_pair_bootstrap("far-apart pair");
	}
}

/* The field case exactly: a locked pair, then one partner reboots (an OTA)
 * and comes back with its clock at zero while the survivor still holds it
 * as a member. Both orders are tried, since which side survives is
 * arbitrary in the field. */
static void test_pair_relocks_after_one_partner_reboots(void)
{
	for (int rebooter = 0; rebooter < 2; rebooter++) {
		iinit(2, 0x5EB0 + (uint32_t)rebooter);
		/* the pair has been running together for hours */
		N[0].clk.theta_us += 19000.0 * 1e6;
		N[1].clk.theta_us += 19000.0 * 1e6;
		irun_s(90);
		assert(ilit(0, 1) == 2);
		struct inode *r = &N[rebooter];

		/* ~75 s dark: MCUboot swap of a 313 KB image, boot, Thread
		 * attach. Longer than the survivor's 45-tick membership grace,
		 * so it forgets the partner and both re-join as strangers. */
		r->alive = false;
		irun_s(75);
		r->alive = true;
		iboot(r);   /* logical clock restarts from zero: 19,000 s behind */
		expect_pair_bootstrap("rebooted partner");
	}
}

/* A crowd still heals a cold newcomer, and the pair rule never fires there:
 * with three or more nodes the snap quorum comes from summed member weights
 * exactly as before. */
static void test_crowd_still_absorbs_cold_newcomer(void)
{
	iinit(4, 0xC401);
	N[3].alive = false;
	irun_s(120);
	assert(ilit(0, 2) == 3);
	N[3].alive = true;
	iboot(&N[3]);
	N[3].clk.theta_us += 19000.0 * 1e6;
	int four_at = -1;

	for (int t = 0; t < 180; t++) {
		irun_s(1);
		if (ilit(0, 3) == 4 &&
		    group_engine_member_count(&N[0].engine) == 4) {
			four_at = t;
			break;
		}
	}
	assert(four_at >= 0);
}

int main(void)
{
	/* The unlucky thread order on the device: the group tick has already
	 * set stranger trust before the first clock update, so nothing snaps
	 * on first contact. The pair rule must not depend on winning that
	 * race. */
	istranger_start = true;
	test_far_apart_pair_locks();
	test_pair_relocks_after_one_partner_reboots();
	test_crowd_still_absorbs_cold_newcomer();
	printf("pair bootstrap tests passed\n");
	return 0;
}
