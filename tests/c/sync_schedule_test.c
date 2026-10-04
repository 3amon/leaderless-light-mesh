#include "group_policy.h"
#include "sync_schedule.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_NODES 24
#define STEP_MS 100
#define GROUP_TICK_MS 1000
#define NEVER INT64_C(-1)

static const struct group_membership_config membership_config = {
	.join_ticks = 20,
	.leave_ticks = 20,
	.grace_ticks = 45,
};

struct sim_node {
	bool online;
	uint32_t random;
	int64_t next_local_ms;
	int64_t next_realm_ms;
	int64_t last_local_ms[MAX_NODES];
	int64_t last_any_ms[MAX_NODES];
	uint32_t peer_view[MAX_NODES];
	struct group_peer_presence presence[MAX_NODES];
	struct group_consensus_policy consensus;
	uint32_t view;
};

struct simulation {
	int count;
	struct sim_node node[MAX_NODES];
	uint64_t local_tx;
	uint64_t realm_tx;
};

static uint32_t random_next(struct sim_node *node)
{
	node->random = node->random * 1664525U + 1013904223U;
	return node->random;
}

static bool packet_delivered(int sender, int receiver, int64_t now_ms,
			     unsigned int loss_percent, unsigned int path)
{
	uint32_t mixed = (uint32_t)now_ms ^ ((uint32_t)sender * 0x9e3779b9U) ^
		((uint32_t)receiver * 0x85ebca6bU) ^ (path * 0xc2b2ae35U);

	mixed ^= mixed >> 16;
	mixed *= 0x7feb352dU;
	mixed ^= mixed >> 15;
	return mixed % 100U >= loss_percent;
}

static void simulation_init(struct simulation *sim, int count)
{
	assert(count >= 2 && count <= MAX_NODES);
	memset(sim, 0, sizeof(*sim));
	sim->count = count;
	for (int i = 0; i < count; i++) {
		struct sim_node *node = &sim->node[i];

		node->online = true;
		node->random = 0x31415927U ^ ((uint32_t)i * 0x9e3779b9U);
		node->view = 1U << i;
		group_consensus_init(&node->consensus, (uint8_t)i);
		for (int peer = 0; peer < count; peer++) {
			node->last_local_ms[peer] = NEVER;
			node->last_any_ms[peer] = NEVER;
			node->peer_view[peer] = 1U << peer;
			group_peer_presence_init(&node->presence[peer]);
		}
		node->next_local_ms = sync_schedule_delay_ms(
			SYNC_SCHEDULE_SCALABLE, SYNC_SCHEDULE_LOCAL,
			random_next(node));
		node->next_realm_ms = sync_schedule_delay_ms(
			SYNC_SCHEDULE_SCALABLE, SYNC_SCHEDULE_REALM,
			random_next(node));
	}
}

static bool directly_connected(int sender, int receiver, int64_t now_ms,
			       int split_until_ms)
{
	if (split_until_ms < 0 || now_ms >= split_until_ms) {
		return true;
	}
	int midpoint = MAX_NODES / 2;

	return (sender < midpoint) == (receiver < midpoint);
}

static void deliver(struct simulation *sim, int sender, bool local,
		    int64_t now_ms, unsigned int loss_percent,
		    int split_until_ms)
{
	for (int receiver = 0; receiver < sim->count; receiver++) {
		if (receiver == sender || !sim->node[receiver].online) {
			continue;
		}
		if (local && !directly_connected(sender, receiver, now_ms,
					 split_until_ms)) {
			continue;
		}
		if (!packet_delivered(sender, receiver, now_ms, loss_percent,
				      local ? 1U : 2U)) {
			continue;
		}
		struct sim_node *target = &sim->node[receiver];

		target->last_any_ms[sender] = now_ms;
		if (local) {
			target->last_local_ms[sender] = now_ms;
		}
		target->peer_view[sender] = sim->node[sender].view;
	}
}

static void transmit_due(struct simulation *sim, int64_t now_ms,
			 unsigned int loss_percent, int split_until_ms)
{
	for (int sender = 0; sender < sim->count; sender++) {
		struct sim_node *node = &sim->node[sender];

		if (!node->online) {
			continue;
		}
		if (now_ms >= node->next_local_ms) {
			sim->local_tx++;
			deliver(sim, sender, true, now_ms, loss_percent,
				split_until_ms);
			node->next_local_ms = now_ms + sync_schedule_delay_ms(
				SYNC_SCHEDULE_SCALABLE, SYNC_SCHEDULE_LOCAL,
				random_next(node));
		}
		if (now_ms >= node->next_realm_ms) {
			sim->realm_tx++;
			deliver(sim, sender, false, now_ms, loss_percent,
				split_until_ms);
			node->next_realm_ms = now_ms + sync_schedule_delay_ms(
				SYNC_SCHEDULE_SCALABLE, SYNC_SCHEDULE_REALM,
				random_next(node));
		}
	}
}

static void group_tick(struct simulation *sim, int64_t now_ms)
{
	for (int self = 0; self < sim->count; self++) {
		struct sim_node *node = &sim->node[self];

		if (!node->online) {
			continue;
		}
		for (int peer = 0; peer < sim->count; peer++) {
			if (peer == self || node->last_any_ms[peer] == NEVER) {
				continue;
			}
			int64_t any_age = now_ms - node->last_any_ms[peer];
			int64_t direct_age = node->last_local_ms[peer] == NEVER
				? INT64_MAX : now_ms - node->last_local_ms[peer];
			bool tracked = any_age <= SYNC_PEER_TIMEOUT_US / 1000;
			bool fresh = any_age < SYNC_OVERALL_FRESH_US / 1000;
			bool direct = direct_age < SYNC_DIRECT_FRESH_US / 1000;

			(void)group_peer_presence_step(
				&node->presence[peer], &membership_config,
				tracked, direct && fresh, fresh, any_age,
				GROUP_TICK_MS);
		}

		uint32_t view = 1U << self;

		for (int peer = 0; peer < sim->count; peer++) {
			if (peer != self && node->presence[peer].membership.state ==
					    GROUP_MEMBER) {
				view |= 1U << peer;
			}
		}
		node->view = view;
	}

	/* Topology consumes the view snapshot from the preceding radio events,
	 * exactly like the shipping one-second group tick. */
	for (int self = 0; self < sim->count; self++) {
		struct sim_node *node = &sim->node[self];
		struct group_topology_node peers[MAX_NODES - 1];
		size_t peer_count = 0;

		if (!node->online) {
			continue;
		}
		for (int peer = 0; peer < sim->count; peer++) {
			if (peer == self || node->last_any_ms[peer] == NEVER ||
			    now_ms - node->last_any_ms[peer] >
				    SYNC_PEER_TIMEOUT_US / 1000) {
				continue;
			}
			peers[peer_count++] = (struct group_topology_node){
				.small_id = (uint8_t)peer,
				.view = node->peer_view[peer],
			};
		}
		struct group_topology_result topology = group_topology_component(
			(uint8_t)self, node->view, peers, peer_count);

		(void)group_consensus_step(&node->consensus, (uint8_t)self,
					   topology.component,
					   topology.complete, 5);
	}
}

static uint32_t online_mask(const struct simulation *sim)
{
	uint32_t mask = 0;

	for (int i = 0; i < sim->count; i++) {
		if (sim->node[i].online) {
			mask |= 1U << i;
		}
	}
	return mask;
}

static bool converged(const struct simulation *sim)
{
	uint32_t expected = online_mask(sim);

	for (int i = 0; i < sim->count; i++) {
		if (!sim->node[i].online) {
			continue;
		}
		if (sim->node[i].view != expected ||
		    sim->node[i].consensus.agreed != expected) {
			return false;
		}
	}
	return true;
}

static int64_t run_until_converged(struct simulation *sim, int64_t start_ms,
				   int64_t deadline_ms,
				   unsigned int loss_percent,
				   int split_until_ms)
{
	for (int64_t now = start_ms; now <= deadline_ms; now += STEP_MS) {
		transmit_due(sim, now, loss_percent, split_until_ms);
		if (now % GROUP_TICK_MS == 0) {
			group_tick(sim, now);
			if (converged(sim)) {
				return now;
			}
		}
	}
	return NEVER;
}

static void test_ranges_and_migration(void)
{
	bool seen_compat[101] = {false};
	bool seen_local[801] = {false};
	bool seen_realm[4001] = {false};

	for (uint32_t random = 0; random < 20000U; random++) {
		uint32_t compat = sync_schedule_delay_ms(
			SYNC_SCHEDULE_COMPAT, SYNC_SCHEDULE_LOCAL, random);
		uint32_t local = sync_schedule_delay_ms(
			SYNC_SCHEDULE_SCALABLE, SYNC_SCHEDULE_LOCAL, random);
		uint32_t realm = sync_schedule_delay_ms(
			SYNC_SCHEDULE_SCALABLE, SYNC_SCHEDULE_REALM, random);

		assert(compat >= 450U && compat <= 550U);
		assert(local >= 1800U && local <= 2600U);
		assert(realm >= 8000U && realm <= 12000U);
		seen_compat[compat - 450U] = true;
		seen_local[local - 1800U] = true;
		seen_realm[realm - 8000U] = true;
	}
	for (size_t i = 0; i < 101U; i++) assert(seen_compat[i]);
	for (size_t i = 0; i < 801U; i++) assert(seen_local[i]);
	for (size_t i = 0; i < 4001U; i++) assert(seen_realm[i]);

	struct sync_compat_policy policy;

	sync_compat_policy_init(&policy, 1000);
	assert(sync_compat_policy_profile(&policy, 120999) ==
	       SYNC_SCHEDULE_COMPAT);
	assert(sync_compat_policy_profile(&policy, 121000) ==
	       SYNC_SCHEDULE_SCALABLE);
	sync_compat_policy_note_wire_version(&policy, 3, 130000);
	assert(sync_compat_policy_profile(&policy, 130000) ==
	       SYNC_SCHEDULE_SCALABLE);
	sync_compat_policy_note_wire_version(&policy, 2, 130000);
	assert(sync_compat_policy_profile(&policy, 249999) ==
	       SYNC_SCHEDULE_COMPAT);
	assert(sync_compat_policy_profile(&policy, 250000) ==
	       SYNC_SCHEDULE_SCALABLE);
}

static void test_scale(int count)
{
	struct simulation sim;

	simulation_init(&sim, count);
	int64_t at = run_until_converged(&sim, 0, 120000, 30U, -1);

	assert(at != NEVER);
	/* Hold another minute under loss: an established group may not flap. */
	for (int64_t now = at + STEP_MS; now <= at + 60000; now += STEP_MS) {
		transmit_due(&sim, now, 30U, -1);
		if (now % GROUP_TICK_MS == 0) {
			group_tick(&sim, now);
			assert(converged(&sim));
		}
	}
	printf("%d nodes converged in %lldms\n", count, (long long)at);
	if (count == 24) {
		double seconds = (double)(at + 60000) / 1000.0;
		double total_rate = (double)(sim.local_tx + sim.realm_tx) / seconds;

		/* Current firmware originates 4*N = 96 datagrams/s at 24.
		 * The scalable profile should remain below 18/s even before loss. */
		assert(total_rate < 18.0);
	}
}

static void test_two_groups_merge(void)
{
	struct simulation sim;

	simulation_init(&sim, 24);
	/* Two established halves meet at 60 s. */
	int64_t at = run_until_converged(&sim, 0, 180000, 20U, 60000);

	assert(at >= 60000 && at <= 180000);
}

static void test_lone_joiner_preserves_incumbent_group(void)
{
	struct simulation sim;

	simulation_init(&sim, 9);
	sim.node[8].online = false;
	int64_t initial = run_until_converged(&sim, 0, 120000, 20U, -1);

	assert(initial != NEVER);
	uint32_t incumbent = (1U << 8) - 1U;
	uint32_t complete = (1U << 9) - 1U;

	sim.node[8].online = true;
	bool finished = false;
	for (int64_t now = initial + STEP_MS; now <= initial + 120000;
	     now += STEP_MS) {
		transmit_due(&sim, now, 20U, -1);
		if (now % GROUP_TICK_MS != 0) {
			continue;
		}
		group_tick(&sim, now);
		for (int i = 0; i < 8; i++) {
			/* The incumbent eight may keep the committed set or accept the
			 * newcomer; they must never fragment into partial/solo masks. */
			assert(sim.node[i].consensus.agreed == incumbent ||
			       sim.node[i].consensus.agreed == complete);
		}
		if (converged(&sim)) {
			finished = true;
			break;
		}
	}
	assert(finished);
}

static void test_burst_blackout_does_not_flap(void)
{
	struct simulation sim;

	simulation_init(&sim, 9);
	int64_t initial = run_until_converged(&sim, 0, 120000, 20U, -1);

	assert(initial != NEVER);
	for (int64_t now = initial + STEP_MS; now <= initial + 15000;
	     now += STEP_MS) {
		transmit_due(&sim, now, 100U, -1);
		if (now % GROUP_TICK_MS == 0) {
			group_tick(&sim, now);
			assert(converged(&sim));
		}
	}
	int64_t recovered = run_until_converged(
		&sim, initial + 15000 + STEP_MS, initial + 75000, 20U, -1);

	assert(recovered != NEVER);
}

static void test_departure(void)
{
	struct simulation sim;

	simulation_init(&sim, 20);
	int64_t initial = run_until_converged(&sim, 0, 120000, 20U, -1);

	assert(initial != NEVER);
	sim.node[19].online = false;
	int64_t departed = run_until_converged(&sim, initial + STEP_MS,
					 initial + 120000, 20U, -1);

	assert(departed != NEVER);
}

int main(void)
{
	test_ranges_and_migration();
	test_scale(9);
	test_scale(16);
	test_scale(20);
	test_scale(24);
	test_two_groups_merge();
	test_lone_joiner_preserves_incumbent_group();
	test_burst_blackout_does_not_flap();
	test_departure();
	puts("sync schedule simulations passed");
	return 0;
}
