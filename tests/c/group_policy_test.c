#include <assert.h>
#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include "group_policy.h"

static const struct group_membership_config membership_config = {
	.join_ticks = 10,
	.leave_ticks = 5,
	.grace_ticks = 15,
};

static void join_peer(struct group_membership_policy *policy)
{
	for (int tick = 0; tick < 9; tick++) {
		assert(group_membership_step(policy, &membership_config, true,
					     true) == GROUP_MEMBERSHIP_NONE);
	}
	assert(group_membership_step(policy, &membership_config, true, true) ==
	       GROUP_MEMBERSHIP_JOINED);
	assert(policy->state == GROUP_MEMBER);
}

static void test_membership_traces(void)
{
	struct group_membership_policy policy;

	group_membership_init(&policy);
	assert(policy.state == GROUP_STRANGER);

	/* A partial approach must restart when direct visibility disappears. */
	for (int tick = 0; tick < 9; tick++) {
		assert(group_membership_step(&policy, &membership_config, true,
					     true) == GROUP_MEMBERSHIP_NONE);
	}
	assert(policy.state == GROUP_CANDIDATE);
	assert(group_membership_step(&policy, &membership_config, false, true) ==
	       GROUP_MEMBERSHIP_NONE);
	assert(policy.state == GROUP_STRANGER);
	join_peer(&policy);

	/* Sustained direct contact remains a member. Signal strength is
	 * deliberately absent from this policy contract. */
	for (int tick = 0; tick < 20; tick++) {
		assert(group_membership_step(&policy, &membership_config, true,
					     true) == GROUP_MEMBERSHIP_NONE);
	}
	assert(policy.state == GROUP_MEMBER);

	/* A recovered direct link resets the short visibility-loss leave run. */
	for (int tick = 0; tick < 4; tick++) {
		assert(group_membership_step(&policy, &membership_config, false,
					     true) == GROUP_MEMBERSHIP_NONE);
	}
	assert(group_membership_step(&policy, &membership_config, true, true) ==
	       GROUP_MEMBERSHIP_NONE);
	for (int tick = 0; tick < 4; tick++) {
		assert(group_membership_step(&policy, &membership_config, false,
					     true) == GROUP_MEMBERSHIP_NONE);
	}
	assert(group_membership_step(&policy, &membership_config, false, true) ==
	       GROUP_MEMBERSHIP_LEFT);

	/* Every silent member gets the longer obstruction grace. */
	join_peer(&policy);
	for (int tick = 0; tick < 14; tick++) {
		assert(group_membership_step(&policy, &membership_config, false,
					     false) == GROUP_MEMBERSHIP_NONE);
	}
	assert(policy.state == GROUP_MEMBER);
	assert(group_membership_step(&policy, &membership_config, true, true) ==
	       GROUP_MEMBERSHIP_NONE);
	for (int tick = 0; tick < 14; tick++) {
		assert(group_membership_step(&policy, &membership_config, false,
					     false) == GROUP_MEMBERSHIP_NONE);
	}
	assert(group_membership_step(&policy, &membership_config, false, false) ==
	       GROUP_MEMBERSHIP_LEFT);
}

static void test_presence_outlives_clock_record(void)
{
	struct group_peer_presence presence;

	group_peer_presence_init(&presence);
	for (int tick = 0; tick < 10; tick++) {
		enum group_membership_change change = group_peer_presence_step(
			&presence, &membership_config, true, true, true, 100,
			1000);

		assert(change == (tick == 9 ? GROUP_MEMBERSHIP_JOINED :
						 GROUP_MEMBERSHIP_NONE));
	}

	/* The clock table exports a stale record for roughly four ticks, then
	 * removes it at six seconds. Departure grace must keep advancing after
	 * that record is no longer observed. */
	for (int tick = 0; tick < 4; tick++) {
		assert(group_peer_presence_step(
			       &presence, &membership_config, true, false, false,
			       2500 + tick * 1000, 1000) ==
		       GROUP_MEMBERSHIP_NONE);
	}
	for (int tick = 0; tick < 10; tick++) {
		assert(group_peer_presence_step(
			       &presence, &membership_config, false, false, false,
			       0, 1000) == GROUP_MEMBERSHIP_NONE);
	}
	assert(presence.membership.state == GROUP_MEMBER);
	assert(group_peer_presence_step(&presence, &membership_config, false,
					false, false, 0, 1000) ==
	       GROUP_MEMBERSHIP_LEFT);
	assert(!presence.visible);
	assert(!presence.fresh);
	assert(presence.age_ms > 6000);

	/* A returning peer starts a new deliberate join run and refreshes age. */
	for (int tick = 0; tick < 10; tick++) {
		enum group_membership_change change = group_peer_presence_step(
			&presence, &membership_config, true, true, true, 25,
			1000);

		assert(change == (tick == 9 ? GROUP_MEMBERSHIP_JOINED :
						 GROUP_MEMBERSHIP_NONE));
	}
	assert(presence.visible);
	assert(presence.fresh);
	assert(presence.age_ms == 25);

	presence.age_ms = INT64_MAX - 500;
	assert(group_peer_presence_step(&presence, &membership_config, false,
					false, false, 0, 1000) ==
	       GROUP_MEMBERSHIP_NONE);
	assert(presence.age_ms == INT64_MAX);
}

static void test_departed_collision_record_retires(void)
{
	struct group_peer_presence departed;

	group_peer_presence_init(&departed);
	join_peer(&departed.membership);
	departed.visible = true;
	departed.fresh = true;
	departed.age_ms = 100;

	/* A still-exported sync record is never retired, even once membership
	 * has left. The sync layer remains the authority on observation life. */
	for (unsigned int tick = 0; tick < membership_config.grace_ticks;
	     tick++) {
		(void)group_peer_presence_step(&departed, &membership_config,
					       true, false, false,
					       45000 + (int64_t)tick * 1000,
					       1000);
	}
	assert(departed.membership.state == GROUP_STRANGER);
	assert(!group_peer_retire_ready(&departed, true));

	/* Once sync's retention window expires, an OLD agreed bit must not pin
	 * the departed record. Removing a malformed/colliding record restores a
	 * complete live topology, after which ordinary damping reconverges. */
	assert(group_peer_retire_ready(&departed, false));
	const uint32_t live = (1U << 1) | (1U << 2) | (1U << 11);
	struct group_topology_node poisoned[] = {
		{.small_id = 2, .view = live},
		{.small_id = 11, .view = live},
		{.small_id = 11, .view = 1U << 11},
	};
	struct group_topology_result topology = group_topology_component(
		1, live, poisoned, 3);

	assert(!topology.complete);
	struct group_topology_node cleaned[] = {
		{.small_id = 2, .view = live},
		{.small_id = 11, .view = live},
	};
	topology = group_topology_component(1, live, cleaned, 2);
	assert(topology.complete);
	assert(topology.component == live);

	struct group_consensus_policy consensus = {
		.agreed = (1U << 0) | (1U << 1) | (1U << 2),
	};
	for (unsigned int tick = 0; tick < 5; tick++) {
		uint32_t agreed = group_consensus_step(&consensus, 1,
							live, true, 5);

		assert(agreed == (tick < 4
			? ((1U << 0) | (1U << 1) | (1U << 2))
			: live));
	}
}

static struct group_topology_result topology_for(
	uint8_t self_id, uint32_t self_view, uint8_t id_a, uint32_t view_a,
	uint8_t id_b, uint32_t view_b)
{
	struct group_topology_node nodes[2] = {
		{.small_id = id_a, .view = view_a},
		{.small_id = id_b, .view = view_b},
	};

	return group_topology_component(self_id, self_view, nodes, 2);
}

static void test_topology_components(void)
{
	const uint32_t trio = (1U << 1) | (1U << 2) | (1U << 3);
	const uint32_t pair_23 = (1U << 2) | (1U << 3);
	struct group_topology_result result;

	result = topology_for(1, trio, 2, trio, 3, trio);
	assert(result.complete);
	assert(result.component == trio);
	result = topology_for(2, trio, 1, trio, 3, trio);
	assert(result.complete);
	assert(result.component == trio);

	/* Connected mutual proximity is one deterministic group even when the
	 * endpoints of a three-node chain are not direct neighbours. */
	const uint32_t view_12 = (1U << 1) | (1U << 2);
	const uint32_t view_123 = trio;
	const uint32_t view_23 = pair_23;

	result = topology_for(1, view_12, 2, view_123, 3, view_23);
	assert(result.complete);
	assert(result.component == trio);
	result = topology_for(3, view_23, 1, view_12, 2, view_123);
	assert(result.complete);
	assert(result.component == trio);

	/* A one-way Opal link cannot make Opal render a crowd while the other
	 * two render a pair. The mutual graph yields solo + pair. */
	result = topology_for(1, trio, 2, pair_23, 3, pair_23);
	assert(result.complete);
	assert(result.component == (1U << 1));
	result = topology_for(2, pair_23, 1, trio, 3, pair_23);
	assert(result.complete);
	assert(result.component == pair_23);
	result = topology_for(3, pair_23, 1, trio, 2, pair_23);
	assert(result.complete);
	assert(result.component == pair_23);

	/* Unknown claimed IDs, duplicate IDs, and malformed views fail closed. */
	struct group_topology_node unknown[] = {
		{.small_id = 2, .view = (1U << 1) | (1U << 2) | (1U << 4)},
	};
	result = group_topology_component(1, view_12, unknown, 1);
	assert(!result.complete);

	struct group_topology_node duplicate[] = {
		{.small_id = 2, .view = view_12},
		{.small_id = 2, .view = view_12},
	};
	result = group_topology_component(1, view_12, duplicate, 2);
	assert(!result.complete);

	struct group_topology_node malformed[] = {
		{.small_id = 2, .view = 1U << 1},
	};
	result = group_topology_component(1, view_12, malformed, 1);
	assert(!result.complete);
}

static void test_topology_is_a_partition(void)
{
	/* Exhaust every directed three-node proximity graph. Reciprocal edges
	 * may split it into solo/pair/trio components, but components must never
	 * overlap: if A selects B, B must derive exactly A's component. */
	const uint8_t ids[3] = {1, 2, 3};

	for (unsigned int graph = 0; graph < 64U; graph++) {
		uint32_t views[3] = {
			1U << ids[0],
			1U << ids[1],
			1U << ids[2],
		};
		unsigned int edge_bit = 0;

		for (unsigned int from = 0; from < 3U; from++) {
			for (unsigned int to = 0; to < 3U; to++) {
				if (from == to) {
					continue;
				}
				if (graph & (1U << edge_bit)) {
					views[from] |= 1U << ids[to];
				}
				edge_bit++;
			}
		}

		struct group_topology_result results[3];

		for (unsigned int self = 0; self < 3U; self++) {
			struct group_topology_node peers[2];
			size_t peer_count = 0;

			for (unsigned int other = 0; other < 3U; other++) {
				if (other == self) {
					continue;
				}
				peers[peer_count++] = (struct group_topology_node){
					.small_id = ids[other],
					.view = views[other],
				};
			}
			results[self] = group_topology_component(
				ids[self], views[self], peers, peer_count);
			assert(results[self].complete);
			assert(results[self].component & (1U << ids[self]));
		}

		for (unsigned int a = 0; a < 3U; a++) {
			for (unsigned int b = 0; b < 3U; b++) {
				if (results[a].component & (1U << ids[b])) {
					assert(results[a].component ==
					       results[b].component);
				}
			}
		}
	}
}

static void test_twenty_node_full_mesh_converges(void)
{
	const unsigned int node_count = 20;
	const uint32_t all_members = (1U << node_count) - 1U;

	for (unsigned int self = 0; self < node_count; self++) {
		struct group_topology_node peers[19];
		size_t peer_count = 0;

		for (unsigned int other = 0; other < node_count; other++) {
			if (other == self) {
				continue;
			}
			peers[peer_count++] = (struct group_topology_node){
				.small_id = (uint8_t)other,
				.view = all_members,
			};
		}
		struct group_topology_result result = group_topology_component(
			(uint8_t)self, all_members, peers, peer_count);

		assert(result.complete);
		assert(result.component == all_members);
	}
}

static void test_consensus_traces(void)
{
	struct group_consensus_policy policy;
	const uint32_t solo = 1U << 1;
	const uint32_t pair = solo | (1U << 2);
	const uint32_t trio = pair | (1U << 3);

	group_consensus_init(&policy, 1);
	assert(policy.agreed == solo);

	/* A valid topology target must remain stable for three ticks. */
	for (int tick = 0; tick < 2; tick++) {
		assert(group_consensus_step(&policy, 1, trio, true, 3) == solo);
	}
	assert(group_consensus_step(&policy, 1, trio, true, 3) == trio);

	/* Incomplete graph snapshots hold the last agreement and reset pending
	 * changes rather than accumulating evidence across a blind interval. */
	for (int tick = 0; tick < 20; tick++) {
		assert(group_consensus_step(&policy, 1, pair, false, 3) == trio);
	}
	for (int tick = 0; tick < 2; tick++) {
		assert(group_consensus_step(&policy, 1, pair, true, 3) == trio);
	}
	assert(group_consensus_step(&policy, 1, pair, true, 3) == pair);

	/* Rejoining is damped identically, and self is always retained. */
	for (int tick = 0; tick < 2; tick++) {
		assert(group_consensus_step(&policy, 1, trio, true, 3) == pair);
	}
	assert(group_consensus_step(&policy, 1, trio, true, 3) == trio);
	for (int tick = 0; tick < 10; tick++) {
		assert(group_consensus_step(&policy, 1, 0, true, 3) ==
		       (tick < 2 ? trio : solo));
	}
}

static void test_lock_traces(void)
{
	struct group_lock_policy policy;

	group_lock_init(&policy);
	for (int tick = 0; tick < 20; tick++) {
		assert(group_lock_step(&policy, &group_render_lock_config, false,
				       0) ==
		       GROUP_LOCK_NONE);
	}
	assert(!policy.lit); /* Missing samples are never zero-offset evidence. */

	assert(group_lock_step(&policy, &group_render_lock_config, true, 80000) ==
	       GROUP_LOCK_NONE);
	for (int tick = 0; tick < 2; tick++) {
		assert(group_lock_step(&policy, &group_render_lock_config, true,
				       79999) ==
		       GROUP_LOCK_NONE);
	}
	assert(group_lock_step(&policy, &group_render_lock_config, true, 79999) ==
	       GROUP_LOCKED);
	assert(policy.lit);

	/* Measured 20-70 ms beacon timestamp excursions are valid acquisition
	 * evidence and must not flap an already-lit group. Exact 80 ms remains
	 * in the dead zone. */
	for (int tick = 0; tick < 20; tick++) {
		assert(group_lock_step(&policy, &group_render_lock_config, true,
				       70000) == GROUP_LOCK_NONE);
	}
	assert(policy.lit);
	for (int tick = 0; tick < 4; tick++) {
		assert(group_lock_step(&policy, &group_render_lock_config, true,
				       80001) ==
		       GROUP_LOCK_NONE);
	}
	assert(group_lock_step(&policy, &group_render_lock_config, true, 80000) ==
	       GROUP_LOCK_NONE);
	assert(policy.lit);

	/* The 80-96 ms band is HOLD territory: a crowd whose worst median
	 * breathes across the acquisition bar keeps its scene indefinitely
	 * instead of flapping (dusty-lotus incident). */
	for (int tick = 0; tick < 30; tick++) {
		assert(group_lock_step(&policy, &group_render_lock_config, true,
				       88000) == GROUP_LOCK_NONE);
	}
	assert(policy.lit);

	/* A real sustained phase error above the 96 ms release level still
	 * unlocks in five ticks. */
	for (int tick = 0; tick < 4; tick++) {
		assert(group_lock_step(&policy, &group_render_lock_config, true,
				       96001) ==
		       GROUP_LOCK_NONE);
	}
	assert(group_lock_step(&policy, &group_render_lock_config, true, 96001) ==
	       GROUP_UNLOCKED);
	assert(!policy.lit);

	for (int tick = 0; tick < 3; tick++) {
		enum group_lock_change change = group_lock_step(
			&policy, &group_render_lock_config, true, 0);

		assert(change == (tick == 2 ? GROUP_LOCKED : GROUP_LOCK_NONE));
	}

	/* An agreed member becoming unavailable is sustained unlock evidence. */
	for (int tick = 0; tick < 4; tick++) {
		assert(group_lock_step(&policy, &group_render_lock_config, false,
				       0) ==
		       GROUP_LOCK_NONE);
	}
	assert(group_lock_step(&policy, &group_render_lock_config, false, 0) ==
	       GROUP_UNLOCKED);
	assert(!policy.lit);

	/* Counters saturate instead of wrapping during long steady states. */
	for (int tick = 0; tick < 100000; tick++) {
		assert(group_lock_step(&policy, &group_render_lock_config, false,
				       0) ==
		       GROUP_LOCK_NONE);
	}
	assert(policy.unlit_run == group_render_lock_config.unlock_ticks);
}

static void assert_certified_handoff(uint32_t committed, uint32_t target,
			     int timing_delay_ticks)
{
	struct group_consensus_policy consensus = {
		.agreed = committed,
	};
	struct group_handoff_policy handoff;

	group_handoff_init(&handoff);
	for (int tick = 0; tick < 3 + timing_delay_ticks; tick++) {
		uint32_t agreed = group_consensus_step(&consensus, 1, target,
						 true, 3);
		bool set_certified = agreed == target;
		bool timing_ready = tick >= 2 + timing_delay_ticks;
		bool pending_ready = set_certified && timing_ready;
		enum group_handoff_decision decision = group_handoff_step(
			&handoff, true, pending_ready, 8);

		assert(decision ==
		       (pending_ready ? GROUP_HANDOFF_NONE :
					GROUP_HANDOFF_HOLD_COMMITTED));
	}
	assert(consensus.agreed == target);
	assert(handoff.uncertified_ticks == 0);
}

static void test_handoff_traces(void)
{
	const uint32_t trio = (1U << 1) | (1U << 2) | (1U << 3);
	const uint32_t trio_plus_singleton = trio | (1U << 4);
	const uint32_t other_trio = (1U << 4) | (1U << 5) | (1U << 6);
	struct group_handoff_policy policy;

	/* A singleton admission becomes certified before the committed-scene
	 * grace expires, so incumbents never need to reveal solo. */
	assert_certified_handoff(trio, trio_plus_singleton, 1);

	/* Two established trios use the same path: each keeps its committed
	 * scene until the common six-member target is certified. */
	assert_certified_handoff(trio, trio | other_trio, 2);

	/* Incomplete or malformed topology is tolerated only for a bounded
	 * interval, then returns to the existing fail-closed behavior. */
	group_handoff_init(&policy);
	for (int tick = 0; tick < 8; tick++) {
		assert(group_handoff_step(&policy, true, false, 8) ==
		       GROUP_HANDOFF_HOLD_COMMITTED);
	}
	assert(group_handoff_step(&policy, true, false, 8) ==
	       GROUP_HANDOFF_EXPIRED);
	assert(policy.uncertified_ticks == 8);

	/* Merely having a timing sample is not certification: a pending member
	 * outside the phase-lock bound keeps the old scene committed. */
	group_handoff_init(&policy);
	for (int tick = 0; tick < 4; tick++) {
		bool sample_exists = true;
		bool phase_in_bounds = false;
		bool pending_ready = sample_exists && phase_in_bounds;

		assert(group_handoff_step(&policy, true, pending_ready, 8) ==
		       GROUP_HANDOFF_HOLD_COMMITTED);
	}
	assert(policy.uncertified_ticks == 4);
	assert(group_handoff_step(&policy, true, true, 8) ==
	       GROUP_HANDOFF_NONE);

	/* A fully certified target still waits behind an existing two-layer
	 * crossfade; once that blend settles the handoff proceeds directly. */
	group_handoff_init(&policy);
	for (int tick = 0; tick < 2; tick++) {
		bool target_certified = true;
		bool current_blend_settled = false;

		assert(group_handoff_step(
			       &policy, true,
			       target_certified && current_blend_settled, 8) ==
		       GROUP_HANDOFF_HOLD_COMMITTED);
	}
	assert(group_handoff_step(&policy, true, true, 8) ==
	       GROUP_HANDOFF_NONE);

	/* Certification and the absence of a committed scene both clear the
	 * pending interval. A zero grace remains immediately fail-closed. */
	assert(group_handoff_step(&policy, true, true, 8) ==
	       GROUP_HANDOFF_NONE);
	assert(policy.uncertified_ticks == 0);
	assert(group_handoff_step(&policy, false, false, 8) ==
	       GROUP_HANDOFF_NONE);
	assert(group_handoff_step(&policy, true, false, 0) ==
	       GROUP_HANDOFF_EXPIRED);
}

static float abs_float(float value)
{
	return value < 0.f ? -value : value;
}

static void test_group_crossfade_solo_weight(void)
{
	/* A normal solo-to-group fade retains the complementary solo layer. */
	assert(abs_float(group_solo_weight(0.25f, 0.f) - 0.75f) <
	       0.0001f);

	/* Complementary old/new group layers leave no individual scene exposed
	 * at the midpoint of a membership transition. */
	assert(abs_float(group_solo_weight(0.4f, 0.6f)) < 0.0001f);
	assert(abs_float(group_solo_weight(0.6f, 0.4f)) < 0.0001f);
	assert(abs_float(group_solo_weight(0.2f, 0.8f)) < 0.0001f);

	/* Interrupted/overlapping fades clamp safely instead of producing a
	 * negative solo weight. */
	assert(abs_float(group_solo_weight(0.8f, 0.7f)) < 0.0001f);
	assert(abs_float(group_solo_weight(0.f, 0.f) - 1.f) < 0.0001f);
}

int main(void)
{
	test_membership_traces();
	test_presence_outlives_clock_record();
	test_departed_collision_record_retires();
	test_topology_components();
	test_topology_is_a_partition();
	test_twenty_node_full_mesh_converges();
	test_consensus_traces();
	test_lock_traces();
	test_handoff_traces();
	test_group_crossfade_solo_weight();
	puts("group policy tests passed");
	return 0;
}
