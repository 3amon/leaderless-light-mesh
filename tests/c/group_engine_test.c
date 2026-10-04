/* Host tests for the extracted group_engine — plus the first cohesion
 * mini-simulator: N engines exchanging real view snapshots over a
 * synthetic radio. The two "characterization" tests at the end reproduce
 * all-solo collapse mechanisms of the CURRENT design on the host; when a
 * redesign lands they are the assertions to flip.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "group_engine.h"

#define SIM_NODES 4

struct sim_node {
	bool alive;
	uint32_t node_id;
	uint32_t seed;
	int64_t delta_us;      /* clock offset every peer measures against it */
	struct group_engine engine;
};

static struct sim_node sim[SIM_NODES];
static int sim_count;
static int64_t sim_now_ms;

static void sim_reset(int count)
{
	/* node ids chosen at runtime so every engine derives a distinct
	 * small id — sid collisions get their own dedicated scenarios. */
	memset(sim, 0, sizeof(sim));
	sim_count = count;
	sim_now_ms = 0;

	uint32_t candidate = 0x1000;

	for (int i = 0; i < count; i++) {
		for (;;) {
			candidate += 0x011f;
			struct group_engine probe;

			group_engine_init(&probe, candidate, candidate ^ 0x5eed);
			bool clash = false;

			for (int j = 0; j < i; j++) {
				if (sim[j].engine.my_sid == probe.my_sid) {
					clash = true;
					break;
				}
			}
			if (!clash) {
				sim[i].node_id = candidate;
				sim[i].seed = candidate ^ 0x5eed;
				sim[i].engine = probe;
				break;
			}
		}
		sim[i].alive = true;
		sim[i].delta_us = 2000 + 137 * i;
	}
}

/* One simulated second: every node receives a fresh, direct, lossless
 * snapshot of every other alive node's current advertisement. */
static void sim_step(struct group_engine_events *events_out /* [SIM_NODES] */)
{
	struct sync_peer snapshot[SIM_NODES][SIM_NODES];
	int counts[SIM_NODES] = {0};

	for (int self = 0; self < sim_count; self++) {
		if (!sim[self].alive) {
			continue;
		}
		for (int other = 0; other < sim_count; other++) {
			if (other == self || !sim[other].alive) {
				continue;
			}
			snapshot[self][counts[self]++] = (struct sync_peer){
				.node_id = sim[other].node_id,
				.seed = sim[other].seed,
				.small_id = sim[other].engine.my_sid,
				.view = sim[other].engine.my_view,
				.fresh = true,
				.direct = true,
				.age_ms = 500,
				.delta_us = sim[other].delta_us,
			};
		}
	}
	sim_now_ms += 1000;
	for (int self = 0; self < sim_count; self++) {
		struct group_engine_events scratch;
		struct group_engine_events *events =
			events_out ? &events_out[self] : &scratch;

		if (!sim[self].alive) {
			memset(events, 0, sizeof(*events));
			continue;
		}
		group_engine_tick(&sim[self].engine, sim_now_ms,
				  snapshot[self], counts[self], NULL, events);
	}
}

static void sim_run(int seconds)
{
	for (int s = 0; s < seconds; s++) {
		sim_step(NULL);
	}
}

static void test_pair_converges_and_mints_identically(void)
{
	sim_reset(2);
	sim_run(40);   /* join 20 + agree 5 + lock 3 with margin */
	assert(group_engine_lit(&sim[0].engine));
	assert(group_engine_lit(&sim[1].engine));
	assert(sim[0].engine.group_seed_cur == sim[1].engine.group_seed_cur);
	assert(group_engine_agreed(&sim[0].engine) ==
	       group_engine_agreed(&sim[1].engine));
	assert(group_engine_member_count(&sim[0].engine) == 2);
	assert(group_engine_tier(&sim[0].engine) == SOCIAL_DUO);
}

static void test_departure_shrinks_group_without_collapse(void)
{
	sim_reset(3);
	sim_run(40);
	for (int i = 0; i < 3; i++) {
		assert(group_engine_lit(&sim[i].engine));
		assert(group_engine_member_count(&sim[i].engine) == 3);
	}

	sim[2].alive = false;
	/* silence grace 45 + agree 5 + handoff margin + retirement window */
	sim_run(160);
	assert(group_engine_lit(&sim[0].engine));
	assert(group_engine_lit(&sim[1].engine));
	assert(group_engine_member_count(&sim[0].engine) == 2);
	assert(sim[0].engine.group_seed_cur == sim[1].engine.group_seed_cur);
	uint32_t departed_bit = 1U << sim[2].engine.my_sid;

	assert((group_engine_agreed(&sim[0].engine) & departed_bit) == 0U);
}

static void test_tick_gate_ignores_subsecond_calls(void)
{
	sim_reset(2);
	sim_run(40);

	struct group_engine_events events;

	group_engine_tick(&sim[0].engine, sim_now_ms + 10, NULL, 0, NULL,
			  &events);
	assert(!events.ticked);
	assert(group_engine_lit(&sim[0].engine));
}

/* Mechanism #1 from the cohesion assessment, closed by the
 * localized-completeness fix: a duplicate small id (sid-squatting rogue,
 * or the stale-collider record that scrambled the fleet before v0.1.68)
 * used to mark topology incomplete for every node in range and darken
 * the whole crowd once the handoff expired. Quarantine now keeps only
 * the canonical (lowest node_id) claimant visible to topology, so the
 * healthy pair stays lit. The rogue's node_id is chosen HIGHER than the
 * victim's so the victim never yields its sid and the duplicate
 * persists for the whole run. */
static void test_characterize_rogue_view_darkens_healthy_pair(void)
{
	sim_reset(2);
	sim_run(40);
	assert(group_engine_lit(&sim[0].engine));
	assert(sim[1].node_id < 0xff000001);

	for (int s = 0; s < 130; s++) {   /* > handoff 60 + unlock margin */
		struct sync_peer rogue = {
			.node_id = 0xff000001,
			.seed = 0xff000001 ^ 0x5eed,
			.small_id = sim[1].engine.my_sid,   /* squat B's sid */
			.view = 1U << sim[1].engine.my_sid,
			.fresh = true,
			.direct = true,
			.age_ms = 500,
			.delta_us = 2000,
		};
		struct sync_peer inputs[2];
		struct group_engine_events events;

		sim_now_ms += 1000;
		for (int self = 0; self < 2; self++) {
			int other = 1 - self;

			inputs[0] = (struct sync_peer){
				.node_id = sim[other].node_id,
				.seed = sim[other].seed,
				.small_id = sim[other].engine.my_sid,
				.view = sim[other].engine.my_view,
				.fresh = true,
				.direct = true,
				.age_ms = 500,
				.delta_us = sim[other].delta_us,
			};
			inputs[1] = rogue;
			group_engine_tick(&sim[self].engine, sim_now_ms,
					  inputs, 2, NULL, &events);
		}
	}

	/* the healthy pair still hears each other perfectly, and stays lit */
	assert(sim[0].engine.topology.complete);
	assert(group_engine_lit(&sim[0].engine));
	assert(group_engine_lit(&sim[1].engine));
	assert(sim[0].engine.group_seed_cur == sim[1].engine.group_seed_cur);
}

/* Mechanism #3, closed by timing demotion: a member with a persistently
 * bad clock used to hold the whole crowd dark through the
 * all-members-under-80ms lock gate. It is now dropped from views after
 * ten bad ticks; the healthy majority re-mints without it (the bounded
 * handoff carries the lit scene through the shrink), and it earns its
 * way back after five good ticks. */
static void test_characterize_one_bad_clock_darkens_crowd(void)
{
	sim_reset(3);
	sim_run(40);
	assert(group_engine_lit(&sim[0].engine));

	sim[2].delta_us = 200000;   /* 200 ms forever */
	sim_run(60);                /* medians + demotion + re-mint margin */

	assert(group_engine_lit(&sim[0].engine));
	assert(group_engine_lit(&sim[1].engine));
	assert(group_engine_member_count(&sim[0].engine) == 2);
	assert(!group_engine_lit(&sim[2].engine));

	sim[2].delta_us = 2274;     /* clock heals -> full crowd again */
	sim_run(60);
	assert(group_engine_lit(&sim[2].engine));
	assert(group_engine_member_count(&sim[0].engine) == 3);
}

static void test_legacy_wire_peer_capped_at_stranger_trust(void)
{
	/* an established member whose beacons are pre-v3 wire (firmware
	 * older than the estimator fixes) keeps its scene seat but its
	 * clock-vote weight is capped: one legacy straggler at weight 1.0
	 * dragged eight modern nodes' thetas around (dusty-lotus). */
	sim_reset(3);
	sim_run(40);
	assert(group_engine_lit(&sim[0].engine));

	struct sync_peer snapshot[2] = {
		{
			.node_id = sim[1].node_id, .seed = sim[1].seed,
			.small_id = sim[1].engine.my_sid,
			.view = sim[1].engine.my_view,
			.fresh = true, .direct = true, .age_ms = 500,
			.delta_us = sim[1].delta_us, .wire_legacy = true,
		},
		{
			.node_id = sim[2].node_id, .seed = sim[2].seed,
			.small_id = sim[2].engine.my_sid,
			.view = sim[2].engine.my_view,
			.fresh = true, .direct = true, .age_ms = 500,
			.delta_us = sim[2].delta_us,
		},
	};
	struct group_engine_events events;

	sim_now_ms += 1000;
	group_engine_tick(&sim[0].engine, sim_now_ms, snapshot, 2, NULL,
			  &events);
	assert(events.ticked);
	assert(sim[0].engine.peer_weight[0] <= 0.1f + 1e-6f);
	assert(sim[0].engine.peer_weight[1] >= 0.99f);
}

int main(void)
{
	test_pair_converges_and_mints_identically();
	test_departure_shrinks_group_without_collapse();
	test_legacy_wire_peer_capped_at_stranger_trust();
	test_tick_gate_ignores_subsecond_calls();
	test_characterize_rogue_view_darkens_healthy_pair();
	test_characterize_one_bad_clock_darkens_crowd();
	printf("group engine tests passed\n");
	return 0;
}
