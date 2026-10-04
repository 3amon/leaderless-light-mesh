#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "peer_slot_policy.h"
#include "sync.h"

#define CAPACITY SYNC_MAX_PEERS
#define FRESH_US 2200000LL

_Static_assert(CAPACITY == 23,
	       "the fleet hard cap is self plus exactly twenty-three peers");

static void test_existing_and_free_slots(void)
{
	struct peer_slot_policy_entry entries[3] = {
		{.used = true, .node_id = 20, .age_us = 10},
		{.used = false},
		{.used = true, .node_id = 30, .age_us = 20},
	};

	assert(peer_slot_policy_select(entries, 3, 30, FRESH_US) == 2);
	assert(peer_slot_policy_select(entries, 3, 25, FRESH_US) == 1);
}

static void test_stalest_peer_is_evicted_deterministically(void)
{
	struct peer_slot_policy_entry entries[3] = {
		{.used = true, .node_id = 10, .age_us = FRESH_US + 1},
		{.used = true, .node_id = 30, .age_us = FRESH_US + 100},
		{.used = true, .node_id = 20, .age_us = FRESH_US + 100},
	};

	assert(peer_slot_policy_select(entries, 3, 25, FRESH_US) == 1);
}

static void insert(struct peer_slot_policy_entry *entries, uint32_t node_id)
{
	int selected = peer_slot_policy_select(entries, CAPACITY, node_id,
					       FRESH_US);

	if (selected >= 0) {
		entries[selected] = (struct peer_slot_policy_entry){
			.used = true,
			.node_id = node_id,
			.age_us = 0,
		};
	}
}

static void test_fresh_overload_is_arrival_order_independent(void)
{
	struct peer_slot_policy_entry ascending[CAPACITY] = {0};
	struct peer_slot_policy_entry descending[CAPACITY] = {0};

	for (uint32_t id = 1; id <= 40; id++) {
		insert(ascending, id);
	}
	for (uint32_t id = 40; id > 0; id--) {
		insert(descending, id);
	}
	for (uint32_t id = 1; id <= CAPACITY; id++) {
		bool found_ascending = false;
		bool found_descending = false;

		for (int slot = 0; slot < CAPACITY; slot++) {
			found_ascending |= ascending[slot].node_id == id;
			found_descending |= descending[slot].node_id == id;
		}
		assert(found_ascending);
		assert(found_descending);
	}
	assert(peer_slot_policy_select(ascending, CAPACITY, 41, FRESH_US) == -1);
}

static void test_twenty_node_fleet_fits_without_replacement(void)
{
	struct peer_slot_policy_entry entries[CAPACITY] = {0};

	for (uint32_t peer = 1; peer < 20; peer++) {
		insert(entries, peer);
	}
	int used = 0;

	for (int slot = 0; slot < CAPACITY; slot++) {
		used += entries[slot].used ? 1 : 0;
	}
	assert(used == 19);
}

static void test_twenty_four_is_the_hard_cap(void)
{
	struct peer_slot_policy_entry entries[CAPACITY] = {0};

	for (uint32_t peer = 1; peer <= CAPACITY; peer++) {
		insert(entries, peer);
	}
	assert(peer_slot_policy_select(entries, CAPACITY, CAPACITY + 1U,
				       FRESH_US) == -1);
	/* An existing member always retains its slot. */
	assert(peer_slot_policy_select(entries, CAPACITY, CAPACITY, FRESH_US) >= 0);
}

int main(void)
{
	test_existing_and_free_slots();
	test_stalest_peer_is_evicted_deterministically();
	test_fresh_overload_is_arrival_order_independent();
	test_twenty_node_fleet_fits_without_replacement();
	test_twenty_four_is_the_hard_cap();
	puts("peer slot policy tests passed");
	return 0;
}
