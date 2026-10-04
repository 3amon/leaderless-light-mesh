/* Campaign batch 3: lower-id sid collision (M10 retry) and
 * handoff-hold interruption. Measurement mode, no asserts. */
#define main cohesion_suite_main
#include "cohesion_sim_test.c"
#undef main

static int lit3(struct sim *s, int lo, int hi)
{
	int lit = 0;

	for (int i = lo; i <= hi && i < s->count; i++)
		if (s->n[i].alive && !s->n[i].rogue &&
		    group_engine_lit(&s->n[i].engine))
			lit++;
	return lit;
}

/* M10r: newcomer with LOWER node_id collides on sid with an incumbent
 * member. Quarantine canonical = lowest id, so the INCUMBENT must
 * repair. Does it? How fast, and does the scene survive? */
static void m10_lower_id_collision(void)
{
	printf("\n[M10r] lower-id newcomer collides with incumbent sid\n");
	struct sim s;

	sim_init(&s, 5, 0xA100);
	s.n[4].alive = false;
	sim_run(&s, 90);

	/* pick incumbent 0's sid; search a LOWER id with the same sid */
	int victim = 0;
	uint8_t vsid = s.n[victim].engine.my_sid;
	uint32_t low = 0;

	for (uint32_t cand = s.n[victim].node_id - 1; cand > 0x100; cand--) {
		struct group_engine probe;

		group_engine_init(&probe, cand, cand ^ 0x5eedU);
		bool taken = false;

		for (int i = 0; i < 4; i++)
			if (i != victim && s.n[i].engine.my_sid == probe.my_sid)
				taken = true;
		if (!taken && probe.my_sid == vsid) {
			low = cand;
			break;
		}
	}
	if (!low) {
		printf("  no colliding lower id found — scenario void\n");
		return;
	}
	printf("  incumbent id=0x%x sid=%u; newcomer id=0x%x (lower)\n",
	       s.n[victim].node_id, vsid, low);
	int pre_members = group_engine_member_count(&s.n[1].engine);

	s.n[4].node_id = low;
	s.n[4].seed = low ^ 0x5eedU;
	s.n[4].sid_round_persisted = 0;
	s.n[4].alive = true;
	node_boot(&s.n[4]);

	int repaired_at = -1, five_at = -1, dark_max = 0, dark_run = 0;

	for (int t = 0; t < 400; t++) {
		sim_second(&s);
		if (repaired_at < 0 &&
		    s.n[victim].engine.my_sid != vsid)
			repaired_at = t;
		int lit = lit3(&s, 0, 3);

		dark_run = lit == 0 ? dark_run + 1 : 0;
		if (dark_run > dark_max)
			dark_max = dark_run;
		if (five_at < 0 && lit3(&s, 0, 4) == 5 &&
		    group_engine_member_count(&s.n[1].engine) == 5)
			five_at = t;
	}
	printf("  incumbent repaired sid at=%s (round=%d)  incumbents max dark run=%ds\n",
	       repaired_at >= 0 ? ({ static char b[16]; sprintf(b, "%ds", repaired_at); b; }) : "NEVER",
	       s.n[victim].engine.sid_round, dark_max);
	printf("  five-scene (was %d) at=%s\n", pre_members,
	       five_at >= 0 ? ({ static char b[16]; sprintf(b, "%ds", five_at); b; }) : "NEVER(400s)");
}

/* H1: joiner dies mid-candidacy/handoff hold — does the hold release
 * cleanly and the crowd re-settle without churn? */
static void h1_joiner_dies_during_hold(void)
{
	printf("\n[H1] joiner dies during admission hold\n");
	for (int die_after = 5; die_after <= 40; die_after += 5) {
		struct sim s;

		sim_init(&s, 5, 0xB100 + die_after);
		s.n[4].alive = false;
		sim_run(&s, 90);
		uint32_t seed0 = s.n[0].engine.group_seed_cur;

		s.n[4].alive = true;
		node_boot(&s.n[4]);
		sim_run(&s, die_after);
		s.n[4].alive = false;

		int dark = 0, mints = 0, max_hold = 0;
		uint32_t last_seed = seed0;

		for (int t = 0; t < 200; t++) {
			sim_second(&s);
			if (lit3(&s, 0, 3) == 0)
				dark++;
			uint32_t g = s.n[0].engine.group_seed_cur;

			if (g != last_seed) {
				mints++;
				last_seed = g;
			}
			int h = s.n[0].engine.handoff.uncertified_ticks;

			if (h > max_hold)
				max_hold = h;
		}
		printf("  die@+%2ds: dark=%ds mints(200s)=%d size=%d max_hold_ticks=%d\n",
		       die_after, dark, mints,
		       group_engine_member_count(&s.n[0].engine), max_hold);
	}
}

/* H2: two nodes join in quick succession — staggered admissions vs
 * one combined handoff; measure mint churn seen by an incumbent */
static void h2_double_join_churn(void)
{
	printf("\n[H2] two joiners 10s apart: admission churn\n");
	struct sim s;

	sim_init(&s, 6, 0xB200);
	s.n[4].alive = false;
	s.n[5].alive = false;
	sim_run(&s, 90);
	uint32_t last_seed = s.n[0].engine.group_seed_cur;
	int mints = 0, six_at = -1;

	s.n[4].alive = true;
	node_boot(&s.n[4]);
	sim_run(&s, 10);
	s.n[5].alive = true;
	node_boot(&s.n[5]);
	for (int t = 0; t < 300; t++) {
		sim_second(&s);
		uint32_t g = s.n[0].engine.group_seed_cur;

		if (g != last_seed) {
			mints++;
			last_seed = g;
		}
		if (six_at < 0 &&
		    group_engine_member_count(&s.n[0].engine) == 6 &&
		    lit3(&s, 0, 5) == 6)
			six_at = t;
	}
	printf("  mints=%d (ideal 1-2)  six-lit at=%s\n", mints,
	       six_at >= 0 ? ({ static char b[16]; sprintf(b, "%ds", six_at); b; }) : "NEVER(300s)");
}

int main(void)
{
	m10_lower_id_collision();
	h1_joiner_dies_during_hold();
	h2_double_join_churn();
	return 0;
}
