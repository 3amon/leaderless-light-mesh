#include "peer_slot_policy.h"

int peer_slot_policy_select(const struct peer_slot_policy_entry *entries,
			    size_t count, uint32_t incoming_id,
			    int64_t fresh_window_us)
{
	int free_slot = -1;
	int stale_slot = -1;
	int largest_id_slot = -1;

	for (size_t i = 0; i < count; i++) {
		const struct peer_slot_policy_entry *entry = &entries[i];

		if (entry->used && entry->node_id == incoming_id) {
			return (int)i;
		}
		if (!entry->used) {
			if (free_slot < 0) {
				free_slot = (int)i;
			}
			continue;
		}
		if (entry->age_us >= fresh_window_us &&
		    (stale_slot < 0 ||
		     entry->age_us > entries[stale_slot].age_us ||
		     (entry->age_us == entries[stale_slot].age_us &&
		      entry->node_id > entries[stale_slot].node_id))) {
			stale_slot = (int)i;
		}
		if (largest_id_slot < 0 ||
		    entry->node_id > entries[largest_id_slot].node_id) {
			largest_id_slot = (int)i;
		}
	}
	if (free_slot >= 0) {
		return free_slot;
	}
	if (stale_slot >= 0) {
		return stale_slot;
	}
	if (largest_id_slot >= 0 &&
	    incoming_id < entries[largest_id_slot].node_id) {
		return largest_id_slot;
	}
	return -1;
}
