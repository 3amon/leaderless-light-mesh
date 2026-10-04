/* Membership/topology edge-case campaign — measurement mode, no asserts.
 * Reuses the cohesion_sim harness (real group_engine per node, real
 * schedules/slot policy/freshness windows) by inclusion. */
#define main cohesion_suite_main
#include "cohesion_sim_test.c"
#undef main

static int lit_total(struct sim *s)
{
	int lit = 0;

	for (int i = 0; i < s->count; i++) {
		if (s->n[i].alive && !s->n[i].rogue &&
		    group_engine_lit(&s->n[i].engine)) {
			lit++;
		}
	}
	return lit;
}

static int member_count_at(struct sim *s, int i)
{
	return group_engine_member_count(&s->n[i].engine);
}

/* M1: join latency for a cold pair under symmetric loss */
static void m1_join_latency_vs_loss(void)
{
	printf("\n[M1] pair join latency vs loss (cap 600s)\n");
	for (int loss = 0; loss <= 60; loss += 10) {
		struct sim s;

		sim_init(&s, 2, 0x1100 + loss);
		for (int i = 0; i < 2; i++)
			for (int j = 0; j < 2; j++)
				s.loss[i][j] = loss;
		int t = 0;

		while (t < 600 && lit_total(&s) < 2) {
			sim_second(&s);
			t++;
		}
		printf("  loss=%2d%%  both_lit_at=%s\n", loss,
		       t < 600 ? ({ static char b[16]; sprintf(b, "%ds", t); b; })
			       : "NEVER(600s)");
	}
}

/* M2: silence grace boundary for an established member */
static void m2_grace_boundary(void)
{
	printf("\n[M2] member silence grace boundary (grace=45 ticks)\n");
	for (int silent = 40; silent <= 50; silent += 2) {
		struct sim s;

		sim_init(&s, 3, 0x2200 + silent);
		sim_run(&s, 60);
		int before = member_count_at(&s, 0);

		s.n[2].alive = false;
		sim_run(&s, silent);
		s.n[2].alive = true;   /* returns WITHOUT reboot (radio gap) */
		sim_run(&s, 30);
		printf("  silent=%2ds  members(before=%d) after_return=%d lit=%d\n",
		       silent, before, member_count_at(&s, 0), lit_total(&s));
	}
}

/* M3: node flapping right at the grace boundary */
static void m3_flap_at_grace(void)
{
	printf("\n[M3] node flapping 43s-off/6s-on for 10 min (grace 45)\n");
	struct sim s;

	sim_init(&s, 4, 0x3300);
	sim_run(&s, 60);
	int transitions = 0, last = member_count_at(&s, 0);
	int core_dark_seconds = 0;

	for (int t = 0; t < 600; t++) {
		int phase = t % 49;

		if (phase == 0) {
			s.n[3].alive = false;
		} else if (phase == 43) {
			s.n[3].alive = true;   /* same engine state (radio gap) */
		}
		sim_second(&s);
		int m = member_count_at(&s, 0);

		if (m != last) {
			transitions++;
			last = m;
		}
		int lit3 = 0;

		for (int i = 0; i < 3; i++)
			if (group_engine_lit(&s.n[i].engine))
				lit3++;
		if (lit3 == 0)
			core_dark_seconds++;
	}
	printf("  scene-size transitions=%d core(3)-all-dark seconds=%d/600\n",
	       transitions, core_dark_seconds);
}

/* M4: mass cold start at fleet scale */
static void m4_mass_cold_start(void)
{
	printf("\n[M4] 24-node mass cold start\n");
	for (int loss = 0; loss <= 20; loss += 20) {
		struct sim s;

		sim_init(&s, 24, 0x4400 + loss);
		for (int i = 0; i < 24; i++)
			for (int j = 0; j < 24; j++)
				s.loss[i][j] = loss;
		int t = 0, full_at = -1;

		while (t < 900) {
			sim_second(&s);
			t++;
			if (full_at < 0 && lit_total(&s) == 24 &&
			    member_count_at(&s, 0) == 24)
				full_at = t;
		}
		int conflicts = 0;

		for (int i = 0; i < 24; i++)
			conflicts += s.n[i].engine.sid_round;
		printf("  loss=%2d%%  all-24-lit-one-scene at=%s  sid_repair_rounds_total=%d  final_lit=%d size@n0=%d\n",
		       loss,
		       full_at > 0 ? ({ static char b[16]; sprintf(b, "%ds", full_at); b; }) : "NEVER(900s)",
		       conflicts, lit_total(&s), member_count_at(&s, 0));
	}
}

/* M5: half the crowd dies at once */
static void m5_mass_leave(void)
{
	printf("\n[M5] 12 of 24 die simultaneously\n");
	struct sim s;

	sim_init(&s, 24, 0x5500);
	sim_run(&s, 240);
	printf("  pre: lit=%d size=%d\n", lit_total(&s), member_count_at(&s, 0));
	for (int i = 12; i < 24; i++)
		s.n[i].alive = false;
	int survivor_dark = 0, resized_at = -1;

	for (int t = 0; t < 300; t++) {
		sim_second(&s);
		int lit = 0;

		for (int i = 0; i < 12; i++)
			if (group_engine_lit(&s.n[i].engine))
				lit++;
		if (lit == 0)
			survivor_dark++;
		if (resized_at < 0 && lit == 12 && member_count_at(&s, 0) == 12)
			resized_at = t;
	}
	printf("  survivors all-dark seconds=%d  re-minted-as-12 at=%ds\n",
	       survivor_dark, resized_at);
}

/* M7: cloned node_id — two devices share one identity */
static void m7_cloned_node_id(void)
{
	printf("\n[M7] cloned node_id joins a lit 4-crowd (600s)\n");
	struct sim s;

	sim_init(&s, 6, 0x7700);
	s.n[5].alive = false;
	sim_run(&s, 60);
	printf("  pre: lit4=%d size=%d\n", lit_total(&s), member_count_at(&s, 0));
	/* node5 becomes a clone of node4: same node_id, different seed and
	 * an engine of its own (fresh boot) — like a bad reprovision */
	s.n[5].alive = true;
	s.n[5].node_id = s.n[4].node_id;
	s.n[5].seed = 0xC10E5EEDU;
	node_boot(&s.n[5]);
	int dark = 0, splits = 0;

	for (int t = 0; t < 600; t++) {
		sim_second(&s);
		int lit = 0;

		for (int i = 0; i < 4; i++)
			if (group_engine_lit(&s.n[i].engine))
				lit++;
		if (lit == 0)
			dark++;
		uint32_t seed0 = s.n[0].engine.group_seed_cur;

		for (int i = 1; i < 4; i++)
			if (group_engine_lit(&s.n[i].engine) &&
			    s.n[i].engine.group_seed_cur != seed0)
				splits++;
	}
	printf("  honest4 all-dark seconds=%d  scene-split observations=%d  final lit(all)=%d size@n0=%d\n",
	       dark, splits, lit_total(&s), member_count_at(&s, 0));
}

/* M8: chain A-B-C with the bridge flapping */
static void m8_bridge_flap(void)
{
	printf("\n[M8] chain A-B-C, bridge B flaps 25s/25s for 10 min\n");
	struct sim s;

	sim_init(&s, 3, 0x8800);
	/* line topology: A-B and B-C direct; A-C never */
	s.direct[0][2] = s.direct[2][0] = false;
	sim_run(&s, 60);
	printf("  pre: lit=%d size@A=%d (expect trio via transitivity)\n",
	       lit_total(&s), member_count_at(&s, 0));
	int a_dark = 0, a_solo = 0;

	for (int t = 0; t < 600; t++) {
		if (t % 50 == 0)
			s.n[1].alive = false;
		else if (t % 50 == 25)
			s.n[1].alive = true;
		sim_second(&s);
		if (!group_engine_lit(&s.n[0].engine))
			a_dark++;
		if (member_count_at(&s, 0) <= 1)
			a_solo++;
	}
	printf("  A dark seconds=%d/600  A solo-size seconds=%d/600\n",
	       a_dark, a_solo);
}

/* M9: 5-node line topology under loss */
static void m9_chain_of_five(void)
{
	printf("\n[M9] 5-node line topology (only i<->i+1 direct)\n");
	for (int loss = 0; loss <= 20; loss += 10) {
		struct sim s;

		sim_init(&s, 5, 0x9900 + loss);
		for (int i = 0; i < 5; i++)
			for (int j = 0; j < 5; j++) {
				s.direct[i][j] = (i != j) && (i - j == 1 || j - i == 1);
				s.loss[i][j] = loss;
			}
		int t = 0, full_at = -1;

		while (t < 600) {
			sim_second(&s);
			t++;
			if (full_at < 0 && lit_total(&s) == 5 &&
			    member_count_at(&s, 2) == 5)
				full_at = t;
		}
		printf("  loss=%2d%%  five-lit-one-scene at=%s  final lit=%d size@mid=%d worstlock@mid=%lldms\n",
		       loss,
		       full_at > 0 ? ({ static char b[16]; sprintf(b, "%ds", full_at); b; }) : "NEVER(600s)",
		       lit_total(&s), member_count_at(&s, 2),
		       (long long)(s.n[2].engine.lock_worst_us / 1000));
	}
}

/* M10: a LOWER node_id arrives claiming the incumbent's sid */
static void m10_lower_id_sid_claim(void)
{
	printf("\n[M10] late lower-id node forces incumbent sid re-derive\n");
	struct sim s;

	sim_init(&s, 5, 0xAA00);
	s.n[4].alive = false;
	sim_run(&s, 60);
	/* craft node4: lower node_id than node1, same sid as node1 */
	s.n[4].node_id = s.n[1].node_id - 0x100;
	s.n[4].seed = s.n[4].node_id ^ 0x5eedU;
	node_boot(&s.n[4]);
	/* force its derived sid to collide by hunting a matching id */
	for (uint32_t cand = s.n[1].node_id - 1; cand > s.n[1].node_id - 0x40000; cand--) {
		struct group_engine probe;

		group_engine_init(&probe, cand, cand);
		if (probe.my_sid == s.n[1].engine.my_sid) {
			s.n[4].node_id = cand;
			s.n[4].seed = cand ^ 0x5eedU;
			node_boot(&s.n[4]);
			break;
		}
	}
	if (s.n[4].engine.my_sid != s.n[1].engine.my_sid) {
		printf("  (no colliding lower id found in range; skipped)\n");
		return;
	}
	s.n[4].alive = true;
	printf("  collision staged: incumbent id=%08x newcomer id=%08x sid=%u\n",
	       s.n[1].node_id, s.n[4].node_id, s.n[4].engine.my_sid);
	int dark = 0, five_at = -1;

	for (int t = 0; t < 300; t++) {
		sim_second(&s);
		int lit = 0;

		for (int i = 0; i < 4; i++)
			if (group_engine_lit(&s.n[i].engine))
				lit++;
		if (lit == 0)
			dark++;
		if (five_at < 0 && lit_total(&s) == 5 && member_count_at(&s, 0) == 5)
			five_at = t;
	}
	printf("  incumbent re-derived to sid=%u round=%d; core dark seconds=%d; five-lit at=%ds\n",
	       s.n[1].engine.my_sid, s.n[1].engine.sid_round, dark, five_at);
}

int main(void)
{
	m1_join_latency_vs_loss();
	m2_grace_boundary();
	m3_flap_at_grace();
	m4_mass_cold_start();
	m5_mass_leave();
	m7_cloned_node_id();
	m8_bridge_flap();
	m9_chain_of_five();
	m10_lower_id_sid_claim();
	return 0;
}
