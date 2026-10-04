/* cohesion_sim — deterministic adversarial simulator for the group engine.
 *
 * Every node runs the REAL group_engine over a modeled sync layer: real
 * beacon schedules (sync_schedule.c), real slot eviction
 * (peer_slot_policy.c), real freshness windows (12 s direct / 45 s any /
 * 90 s retention), a per-link lossy asymmetric radio, partitions, node
 * flapping, reboots, rogue beacon injection, and per-node clock offsets.
 * Seeded and fully deterministic: a failure reproduces forever.
 *
 * Scenarios assert the DESIRED cohesion contract (docs/
 * sync-cohesion-investigation.md invariants I1-I4). Scenarios that the
 * v0.1.70 engine could not pass are the evidence that motivated the
 * localized-completeness, timing-demotion, and sid-space fixes.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "group_engine.h"
#include "peer_slot_policy.h"
#include "sync_schedule.h"

#define MAX_NODES 27

struct sync_slot {
	bool used;
	uint32_t node_id;
	int64_t last_any_us;
	int64_t last_direct_us;
	uint32_t seed;
	uint8_t small_id;
	uint32_t view;
	int64_t delta_us;
};

struct node {
	bool alive;
	bool rogue;              /* transmits crafted beacons, runs no engine */
	uint8_t rogue_sid;
	uint32_t rogue_view;
	uint32_t node_id;
	uint32_t seed;
	int64_t clock_offset_us; /* truth: this node's clock error */
	uint32_t rng;
	int64_t next_local_ms, next_realm_ms;
	int partition;           /* realm floods stay within a partition */
	struct sync_slot table[SYNC_MAX_PEERS];
	struct group_engine engine;
	int sid_round_persisted; /* survives reboots (models NVS round) */
	/* metrics */
	int64_t solo_since_ms;   /* -1 when lit */
	int64_t max_solo_ms;
};

struct sim {
	int count;
	struct node n[MAX_NODES];
	int64_t now_ms;
	unsigned int loss[MAX_NODES][MAX_NODES]; /* percent, directional */
	bool direct[MAX_NODES][MAX_NODES];      /* one-radio-hop adjacency */
	uint32_t scenario_salt;
};

static uint32_t lcg(uint32_t *state)
{
	*state = *state * 1664525U + 1013904223U;
	return *state;
}

static bool delivered(const struct sim *s, int tx, int rx, unsigned int path)
{
	uint32_t mixed = (uint32_t)s->now_ms ^ ((uint32_t)tx * 0x9e3779b9U) ^
		((uint32_t)rx * 0x85ebca6bU) ^ (path * 0xc2b2ae35U) ^
		s->scenario_salt;

	mixed ^= mixed >> 16;
	mixed *= 0x7feb352dU;
	mixed ^= mixed >> 15;
	return mixed % 100U >= s->loss[tx][rx];
}

/* deterministic beacon-timestamp jitter, 0..2999 us */
static int64_t jitter_us(const struct sim *s, int tx, int rx)
{
	uint32_t mixed = (uint32_t)s->now_ms ^ ((uint32_t)tx << 8) ^
		((uint32_t)rx << 16) ^ 0x51ed270bU;

	mixed *= 0x9e3779b9U;
	mixed ^= mixed >> 13;
	return (int64_t)(mixed % 3000U);
}

static void node_boot(struct node *n)
{
	group_engine_init(&n->engine, n->node_id, n->seed);
	if (n->sid_round_persisted > 0) {
		group_engine_restore_sid_round(&n->engine,
					       n->sid_round_persisted);
	}
	n->next_local_ms = 0;
	n->next_realm_ms = 0;
	n->solo_since_ms = -1;
}

static void sim_init(struct sim *s, int count, uint32_t salt)
{
	memset(s, 0, sizeof(*s));
	s->count = count;
	s->scenario_salt = salt;

	uint32_t candidate = 0xA000 + salt % 977U;

	for (int i = 0; i < count; i++) {
		struct node *n = &s->n[i];

		candidate += 0x02e7;
		n->node_id = candidate;
		n->seed = candidate ^ 0x5eedU;
		n->alive = true;
		n->rng = candidate ^ 0x31415927U;
		n->clock_offset_us = (int64_t)(i * 700);
		node_boot(n);
		for (int j = 0; j < MAX_NODES; j++) {
			s->direct[i][j] = i != j;
		}
	}
}

static void rx_beacon(struct sim *s, int rx, uint32_t node_id, uint32_t seed,
		      uint8_t small_id, uint32_t view, int64_t delta_us,
		      bool direct_path)
{
	struct node *r = &s->n[rx];
	int64_t now_us = s->now_ms * 1000;
	struct peer_slot_policy_entry entries[SYNC_MAX_PEERS];

	for (int k = 0; k < SYNC_MAX_PEERS; k++) {
		entries[k] = (struct peer_slot_policy_entry){
			.used = r->table[k].used,
			.node_id = r->table[k].node_id,
			.age_us = r->table[k].used
				? now_us - r->table[k].last_any_us
				: -1,
		};
	}
	int slot = peer_slot_policy_select(entries, SYNC_MAX_PEERS, node_id,
					   SYNC_OVERALL_FRESH_US);

	if (slot < 0) {
		return;
	}
	struct sync_slot *t = &r->table[slot];

	if (!t->used || t->node_id != node_id) {
		memset(t, 0, sizeof(*t));
		t->used = true;
		t->node_id = node_id;
		t->last_direct_us = -SYNC_DIRECT_FRESH_US;
	}
	t->last_any_us = now_us;
	if (direct_path) {
		t->last_direct_us = now_us;
	}
	t->seed = seed;
	t->small_id = small_id;
	t->view = view;
	t->delta_us = delta_us;
}

static void tx_beacon(struct sim *s, int tx, bool local_path)
{
	struct node *sender = &s->n[tx];

	for (int rx = 0; rx < s->count; rx++) {
		struct node *r = &s->n[rx];

		if (rx == tx || !r->alive || r->rogue) {
			continue;
		}
		if (s->n[tx].partition != r->partition) {
			continue;
		}
		if (local_path && !s->direct[tx][rx]) {
			continue;
		}
		if (!delivered(s, tx, rx, local_path ? 1U : 2U)) {
			continue;
		}
		int64_t delta = sender->clock_offset_us - r->clock_offset_us;

		if (delta < 0) {
			delta = -delta;
		}
		delta += jitter_us(s, tx, rx);
		if (sender->rogue) {
			rx_beacon(s, rx, sender->node_id, sender->seed,
				  sender->rogue_sid, sender->rogue_view, delta,
				  local_path);
		} else {
			rx_beacon(s, rx, sender->node_id, sender->seed,
				  sender->engine.my_sid, sender->engine.my_view,
				  delta, local_path);
		}
	}
}

static void sim_second(struct sim *s)
{
	s->now_ms += 1000;

	/* beacon schedules (real generator, scalable profile) */
	for (int i = 0; i < s->count; i++) {
		struct node *n = &s->n[i];

		if (!n->alive) {
			continue;
		}
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

	/* engine ticks over each node's modeled sync snapshot */
	for (int i = 0; i < s->count; i++) {
		struct node *n = &s->n[i];

		if (!n->alive || n->rogue) {
			continue;
		}
		struct sync_peer sp[SYNC_MAX_PEERS];
		int cnt = 0;
		int64_t now_us = s->now_ms * 1000;

		for (int k = 0; k < SYNC_MAX_PEERS; k++) {
			struct sync_slot *t = &n->table[k];

			if (!t->used) {
				continue;
			}
			int64_t age = now_us - t->last_any_us;

			if (age > SYNC_PEER_TIMEOUT_US) {
				t->used = false;
				continue;
			}
			sp[cnt++] = (struct sync_peer){
				.node_id = t->node_id,
				.seed = t->seed,
				.small_id = t->small_id,
				.view = t->view,
				.fresh = age < SYNC_OVERALL_FRESH_US,
				.direct = (now_us - t->last_direct_us) <
					  SYNC_DIRECT_FRESH_US,
				.age_ms = age / 1000,
				.delta_us = t->delta_us,
			};
		}
		struct group_engine_events events;

		group_engine_tick(&n->engine, s->now_ms, sp, cnt, NULL,
				  &events);
		if (events.sid_conflict) {
			n->sid_round_persisted = n->engine.sid_round;
		}

		/* metrics */
		if (group_engine_lit(&n->engine)) {
			n->solo_since_ms = -1;
		} else {
			if (n->solo_since_ms < 0) {
				n->solo_since_ms = s->now_ms;
			}
			int64_t solo = s->now_ms - n->solo_since_ms;

			if (solo > n->max_solo_ms) {
				n->max_solo_ms = solo;
			}
		}
	}
}

static void sim_run(struct sim *s, int seconds)
{
	for (int t = 0; t < seconds; t++) {
		sim_second(s);
	}
}

static int lit_count(const struct sim *s, int lo, int hi)
{
	int lit = 0;

	for (int i = lo; i <= hi; i++) {
		if (s->n[i].alive && !s->n[i].rogue &&
		    group_engine_lit(&s->n[i].engine)) {
			lit++;
		}
	}
	return lit;
}

static bool same_seed(const struct sim *s, int lo, int hi)
{
	uint32_t seed = 0;
	bool have = false;

	for (int i = lo; i <= hi; i++) {
		if (!s->n[i].alive || s->n[i].rogue) {
			continue;
		}
		if (!have) {
			seed = s->n[i].engine.group_seed_cur;
			have = true;
		} else if (s->n[i].engine.group_seed_cur != seed) {
			return false;
		}
	}
	return true;
}

/* ---------------- scenarios ---------------- */

static void test_nine_nodes_lossless_converge(void)
{
	struct sim s;

	sim_init(&s, 9, 1);
	sim_run(&s, 60);
	assert(lit_count(&s, 0, 8) == 9);
	assert(same_seed(&s, 0, 8));
	assert(group_engine_member_count(&s.n[0].engine) == 9);
}

static void test_nine_nodes_lossy_stay_lit(void)
{
	struct sim s;

	sim_init(&s, 9, 2);
	for (int i = 0; i < 9; i++) {
		for (int j = 0; j < 9; j++) {
			s.loss[i][j] = 20;
		}
	}
	sim_run(&s, 120);
	assert(lit_count(&s, 0, 8) == 9);
	/* ten lossy minutes: I1 — never all-solo */
	for (int minute = 0; minute < 10; minute++) {
		sim_run(&s, 60);
		assert(lit_count(&s, 0, 8) > 0);
	}
	assert(lit_count(&s, 0, 8) == 9);
	assert(same_seed(&s, 0, 8));
}

static void test_asymmetric_links_converge(void)
{
	struct sim s;

	sim_init(&s, 5, 3);
	s.loss[0][1] = 60;   /* A hears B badly, B hears A fine */
	s.loss[2][3] = 50;
	s.loss[3][2] = 10;
	sim_run(&s, 180);
	assert(lit_count(&s, 0, 4) == 5);
	assert(same_seed(&s, 0, 4));
}

static void test_flapping_node_cannot_shatter_core(void)
{
	struct sim s;

	sim_init(&s, 9, 4);
	sim_run(&s, 60);
	assert(lit_count(&s, 0, 8) == 9);

	/* node 8 flaps 30 s off / 30 s on for ten minutes */
	int core_lit_seconds = 0;

	for (int t = 0; t < 600; t++) {
		if (t % 60 == 0) {
			s.n[8].alive = false;
		} else if (t % 60 == 30) {
			s.n[8].alive = true;
			node_boot(&s.n[8]);
		}
		sim_second(&s);
		if (lit_count(&s, 0, 7) == 8) {
			core_lit_seconds++;
		}
		assert(lit_count(&s, 0, 7) > 0);   /* I1 for the core */
	}
	/* the 8-node core must ride through the churn nearly always */
	assert(core_lit_seconds > 540);
}

static void test_rolling_reboots_keep_crowd_lit(void)
{
	struct sim s;

	sim_init(&s, 8, 5);
	sim_run(&s, 60);
	assert(lit_count(&s, 0, 7) == 8);

	for (int victim = 0; victim < 8; victim++) {
		s.n[victim].alive = false;
		sim_run(&s, 3);
		s.n[victim].alive = true;
		node_boot(&s.n[victim]);
		sim_run(&s, 27);
		/* I1: survivors never collapse */
		assert(lit_count(&s, 0, 7) >= 6);
	}
	sim_run(&s, 120);
	assert(lit_count(&s, 0, 7) == 8);
	assert(same_seed(&s, 0, 7));
}

static void test_sid_squat_rogue_cannot_darken_crowd(void)
{
	struct sim s;

	sim_init(&s, 5, 6);
	sim_run(&s, 60);
	assert(lit_count(&s, 0, 3) >= 4);

	/* node 4 becomes a rogue squatting node 1's sid, with a node_id
	 * high enough that node 1 never yields */
	s.n[4].rogue = true;
	s.n[4].node_id = 0xfffffff0U;
	s.n[4].rogue_sid = s.n[1].engine.my_sid;
	s.n[4].rogue_view = 1U << s.n[1].engine.my_sid;

	sim_run(&s, 200);
	/* I1: the healthy four stay lit together despite the squatter */
	assert(lit_count(&s, 0, 3) == 4);
	assert(same_seed(&s, 0, 3));
}

static void test_bad_clock_member_is_demoted_not_fatal(void)
{
	struct sim s;

	sim_init(&s, 5, 7);
	sim_run(&s, 60);
	assert(lit_count(&s, 0, 4) == 5);

	/* node 4's clock goes 200 ms out, persistently */
	s.n[4].clock_offset_us = 200000;
	sim_run(&s, 180);
	/* I1/I4: the healthy four re-mint and stay lit; the outlier is out */
	assert(lit_count(&s, 0, 3) == 4);
	assert(same_seed(&s, 0, 3));
	assert(!group_engine_lit(&s.n[4].engine));
	assert(group_engine_member_count(&s.n[0].engine) == 4);

	/* clock heals -> full crowd again */
	s.n[4].clock_offset_us = 2800;
	sim_run(&s, 240);
	assert(lit_count(&s, 0, 4) == 5);
	assert(group_engine_member_count(&s.n[0].engine) == 5);
}

static void test_overload_26_nodes_stable_core(void)
{
	struct sim s;

	sim_init(&s, 26, 8);
	sim_run(&s, 300);

	/* the retained (lowest-node-id) crowd must be lit and agreed; the
	 * two excluded highest-id nodes end solo but not disruptive */
	int lit = lit_count(&s, 0, 25);

	assert(lit >= 22);
	for (int minute = 0; minute < 5; minute++) {
		sim_run(&s, 60);
		assert(lit_count(&s, 0, 25) >= 22);
	}
}

static void test_partition_and_merge(void)
{
	struct sim s;

	sim_init(&s, 8, 9);
	sim_run(&s, 60);
	assert(lit_count(&s, 0, 7) == 8);

	/* split 4/4 */
	for (int i = 0; i < 8; i++) {
		s.n[i].partition = i < 4 ? 0 : 1;
	}
	sim_run(&s, 180);
	assert(lit_count(&s, 0, 3) == 4);
	assert(lit_count(&s, 4, 7) == 4);
	assert(same_seed(&s, 0, 3));
	assert(same_seed(&s, 4, 7));
	assert(s.n[0].engine.group_seed_cur != s.n[4].engine.group_seed_cur);

	/* merge */
	for (int i = 0; i < 8; i++) {
		s.n[i].partition = 0;
	}
	sim_run(&s, 180);
	assert(lit_count(&s, 0, 7) == 8);
	assert(same_seed(&s, 0, 7));
	assert(group_engine_member_count(&s.n[0].engine) == 8);
}

int main(void)
{
	test_nine_nodes_lossless_converge();
	test_nine_nodes_lossy_stay_lit();
	test_asymmetric_links_converge();
	test_flapping_node_cannot_shatter_core();
	test_rolling_reboots_keep_crowd_lit();
	test_sid_squat_rogue_cannot_darken_crowd();
	test_bad_clock_member_is_demoted_not_fatal();
	test_overload_26_nodes_stable_core();
	test_partition_and_merge();
	printf("cohesion simulator scenarios passed\n");
	return 0;
}
