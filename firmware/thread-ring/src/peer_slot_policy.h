/* Pure bounded-peer-table replacement policy; no Zephyr dependencies. */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct peer_slot_policy_entry {
	bool used;
	uint32_t node_id;
	int64_t age_us;
};

/* Resolve an existing/free slot, evict the stalest retained peer, or—when an
 * overloaded table is entirely fresh—keep the deterministically lowest node
 * IDs. Returns -1 only when the incoming peer loses that deterministic rank. */
int peer_slot_policy_select(const struct peer_slot_policy_entry *entries,
			    size_t count, uint32_t incoming_id,
			    int64_t fresh_window_us);
