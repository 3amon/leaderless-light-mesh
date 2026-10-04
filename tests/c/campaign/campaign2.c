/* Campaign batch 2: refined instrumentation for the anomalies batch 1
 * surfaced, plus slot exhaustion, sid-31, and handoff expiry. */
#define main cohesion_suite_main
#include "cohesion_sim_test.c"
#undef main


static int lit_total2(struct sim *s)
{
	int lit = 0;

	for (int i = 0; i < s->count; i++)
		if (s->n[i].alive && !s->n[i].rogue &&
		    group_engine_lit(&s->n[i].engine))
			lit++;
	return lit;
}

/* engine tick wrapper that counts events at one observer */
static struct group_engine_events last_events;

static void sim_second_observed(struct sim *s, int observer, int *mints,
				int *leaves, int *joins)
{
	/* replicate sim_second but capture observer's events */
	s->now_ms += 1000;
	for (int i = 0; i < s->count; i++) {
		struct node *n = &s->n[i];

		if (!n->alive)
			continue;
		if (s->now_ms >= n->next_local_ms) {
			tx_beacon(s, i, true);
			n->next_local_ms = s->now_ms + sync_schedule_delay_ms(
				SYNC_SCHEDULE_SCALABLE, SYNC_SCHEDULE_LOCAL,
				lcg(&n->rng));
		}
		if (s->now_ms >= n->next_realm_ms) {
			tx_beacon(s, i, false);
			n->next_realm_ms = s->now_ms + sync_schedule_delay_ms(
				SYNC_SCHEDULE_SCALABLE, SYNC_SCHEDULE_REALM,
				lcg(&n->rng));
		}
	}
	for (int i = 0; i < s->count; i++) {
		struct node *n = &s->n[i];

		if (!n->alive || n->rogue)
			continue;
		struct sync_peer sp[SYNC_MAX_PEERS];
		int cnt = 0;
		int64_t now_us = s->now_ms * 1000;

		for (int k = 0; k < SYNC_MAX_PEERS; k++) {
			struct sync_slot *t = &n->table[k];

			if (!t->used)
				continue;
			int64_t age = now_us - t->last_any_us;

			if (age > SYNC_PEER_TIMEOUT_US) {
				t->used = false;
				continue;
			}
			sp[cnt++] = (struct sync_peer){
				.node_id = t->node_id, .seed = t->seed,
				.small_id = t->small_id, .view = t->view,
				.fresh = age < SYNC_OVERALL_FRESH_US,
				.direct = (now_us - t->last_direct_us) <
					  SYNC_DIRECT_FRESH_US,
				.age_ms = age / 1000,
				.delta_us = t->delta_us,
			};
		}
		struct group_engine_events ev;

		group_engine_tick(&n->engine, s->now_ms, sp, cnt, NULL, &ev);
		if (i == observer && ev.ticked) {
			last_events = ev;
			if (mints && ev.minted)
				(*mints)++;
			if (leaves)
				(*leaves) += ev.leave_count;
			if (joins)
				(*joins) += ev.join_count;
		}
	}
}

/* R2: exact silent-departure timing */
static void r2_exact_leave_timing(void)
{
	printf("\n[R2] exact leave tick for a fully silent member (docs claim 45s grace)\n");
	struct sim s;

	sim_init(&s, 3, 0xB200);
	sim_run(&s, 60);
	s.n[2].alive = false;
	for (int t = 1; t <= 90; t++) {
		int leaves = 0;

		sim_second_observed(&s, 0, NULL, &leaves, NULL);
		if (leaves) {
			printf("  observer saw LEAVE after %d s of silence\n", t);
			return;
		}
	}
	printf("  no leave within 90 s (?)\n");
}

/* R7: clone identity — scene re-mint churn with self-echo fidelity */
static void r7_clone_churn(void)
{
	printf("\n[R7] cloned node_id: mint churn at an honest observer (600s)\n");
	struct sim s;

	sim_init(&s, 6, 0xB700);
	s.n[5].alive = false;
	sim_run(&s, 60);
	s.n[5].alive = true;
	s.n[5].node_id = s.n[4].node_id;
	s.n[5].seed = 0xC10E5EEDU;
	node_boot(&s.n[5]);
	/* self-echo fidelity: firmware drops beacons whose node_id == own id,
	 * so the two clones never hear each other */
	int mints = 0;

	for (int t = 0; t < 600; t++) {
		/* suppress clone-to-clone delivery */
		sim_second_observed(&s, 0, &mints, NULL, NULL);
		/* undo any clone->clone table entry (self-echo filter) */
		for (int c = 4; c <= 5; c++)
			for (int k = 0; k < SYNC_MAX_PEERS; k++)
				if (s.n[c].table[k].used &&
				    s.n[c].table[k].node_id == s.n[c].node_id)
					s.n[c].table[k].used = false;
	}
	printf("  scene mints at observer over 600s: %d (steady crowd would be ~10 from minute rotation)\n",
	       mints);
}

/* R6: slot exhaustion churn — 26 nodes, then two low-id members die */
static void r6_slot_exhaustion(void)
{
	printf("\n[R6] 26 nodes vs 23-slot tables; then 2 lowest-id nodes die\n");
	struct sim s;

	sim_init(&s, 26, 0xB600);
	sim_run(&s, 300);
	int lit = 0, in_scene = 0;

	for (int i = 0; i < 26; i++)
		if (group_engine_lit(&s.n[i].engine))
			lit++;
	printf("  t=300: lit=%d size@n0=%d size@n25(high id)=%d\n", lit,
	       group_engine_member_count(&s.n[0].engine),
	       group_engine_member_count(&s.n[25].engine));
	s.n[0].alive = false;
	s.n[1].alive = false;
	int promoted_at = -1;

	for (int t = 0; t < 400; t++) {
		sim_second_observed(&s, 25, NULL, NULL, NULL);
		if (promoted_at < 0 &&
		    group_engine_lit(&s.n[25].engine) &&
		    group_engine_member_count(&s.n[25].engine) >= 24)
			promoted_at = t;
	}
	printf("  after 2 low-id deaths: high-id node in-scene at=%s size@n25=%d\n",
	       promoted_at >= 0 ? ({ static char b[16]; sprintf(b, "%ds", promoted_at); b; }) : "NEVER(400s)",
	       group_engine_member_count(&s.n[25].engine));
}

/* R11: does sid 31 mint correctly (bitmask edge) */
static void r11_sid31(void)
{
	printf("\n[R11] sid-31 boundary\n");
	/* find a node id whose sid is 31 */
	uint32_t id31 = 0;

	for (uint32_t cand = 0x9000; cand < 0x200000; cand += 7) {
		struct group_engine probe;

		group_engine_init(&probe, cand, cand);
		if (probe.my_sid == 31) {
			id31 = cand;
			break;
		}
	}
	if (!id31) {
		printf("  no sid-31 id found (?)\n");
		return;
	}
	struct sim s;

	sim_init(&s, 3, 0xBB00);
	s.n[2].node_id = id31;
	s.n[2].seed = id31 ^ 0x5eed;
	node_boot(&s.n[2]);
	sim_run(&s, 60);
	printf("  trio with sid31 member: lit=%d size=%d agreed@n0=%08x (bit31 %s)\n",
	       lit_total2(&s), group_engine_member_count(&s.n[0].engine),
	       group_engine_agreed(&s.n[0].engine),
	       (group_engine_agreed(&s.n[0].engine) & 0x80000000u) ? "SET" : "unset");
}

/* R1b: join latency cliff hunt at extreme loss */
static void r1b_loss_cliff(void)
{
	printf("\n[R1b] pair join at extreme loss (cap 900s)\n");
	for (int loss = 70; loss <= 90; loss += 5) {
		struct sim s;

		sim_init(&s, 2, 0xB100 + loss);
		for (int i = 0; i < 2; i++)
			for (int j = 0; j < 2; j++)
				s.loss[i][j] = loss;
		int t = 0;

		while (t < 900 && lit_total2(&s) < 2) {
			sim_second(&s);
			t++;
		}
		printf("  loss=%2d%%  both_lit_at=%s\n", loss,
		       t < 900 ? ({ static char b[16]; sprintf(b, "%ds", t); b; }) : "NEVER(900s)");
	}
}

int main(void)
{
	r2_exact_leave_timing();
	r7_clone_churn();
	r6_slot_exhaustion();
	r11_sid31();
	r1b_loss_cliff();
	return 0;
}
