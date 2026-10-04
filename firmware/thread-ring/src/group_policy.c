#include "group_policy.h"

#include <limits.h>
#include <string.h>

const struct group_lock_config group_render_lock_config = {
	.lock_on_us = 80000,
	.lock_off_us = 96000,
	.lock_ticks = 3,
	.unlock_ticks = 5,
};

static unsigned int increment_saturated(unsigned int value,
					unsigned int limit)
{
	return value < limit ? value + 1U : limit;
}

void group_membership_init(struct group_membership_policy *policy)
{
	memset(policy, 0, sizeof(*policy));
}

enum group_membership_change group_membership_step(
	struct group_membership_policy *policy,
	const struct group_membership_config *config, bool visible, bool fresh)
{
	switch (policy->state) {
	case GROUP_STRANGER:
	case GROUP_CANDIDATE:
		if (visible && fresh) {
			policy->state = GROUP_CANDIDATE;
			policy->run = increment_saturated(policy->run,
						  config->join_ticks);
			if (policy->run >= config->join_ticks) {
				policy->state = GROUP_MEMBER;
				policy->run = 0;
				return GROUP_MEMBERSHIP_JOINED;
			}
		} else {
			policy->state = GROUP_STRANGER;
			policy->run = 0;
		}
		break;
	case GROUP_MEMBER:
		if (!visible || !fresh) {
			unsigned int limit = !fresh ? config->grace_ticks
						   : config->leave_ticks;

			policy->run = increment_saturated(policy->run, limit);
			if (policy->run >= limit) {
				policy->state = GROUP_STRANGER;
				policy->run = 0;
				return GROUP_MEMBERSHIP_LEFT;
			}
		} else {
			policy->run = 0;
		}
		break;
	}

	return GROUP_MEMBERSHIP_NONE;
}

void group_peer_presence_init(struct group_peer_presence *presence)
{
	memset(presence, 0, sizeof(*presence));
	group_membership_init(&presence->membership);
}

enum group_membership_change group_peer_presence_step(
	struct group_peer_presence *presence,
	const struct group_membership_config *config, bool observed,
	bool observed_visible, bool observed_fresh, int64_t observed_age_ms,
	int64_t elapsed_ms)
{
	if (observed) {
		presence->visible = observed_visible;
		presence->fresh = observed_fresh;
		presence->age_ms = observed_age_ms > 0 ? observed_age_ms : 0;
	} else {
		presence->visible = false;
		presence->fresh = false;
		int64_t advance = elapsed_ms > 0 ? elapsed_ms : 0;

		if (presence->age_ms > INT64_MAX - advance) {
			presence->age_ms = INT64_MAX;
		} else {
			presence->age_ms += advance;
		}
	}

	return group_membership_step(&presence->membership, config,
				     presence->visible, presence->fresh);
}

bool group_peer_retire_ready(const struct group_peer_presence *presence,
			     bool observed)
{
	return presence != NULL && !observed && !presence->visible &&
	       !presence->fresh &&
	       presence->membership.state == GROUP_STRANGER;
}

struct group_topology_result group_topology_component(
	uint8_t self_id, uint32_t self_view,
	const struct group_topology_node *nodes, size_t node_count)
{
	struct group_topology_result result = {
		.component = 0,
		.complete = true,
	};
	uint32_t views[32] = {0};
	bool present[32] = {false};

	if (self_id >= 32U) {
		result.complete = false;
		return result;
	}

	uint32_t self_bit = 1U << self_id;

	if (!(self_view & self_bit)) {
		result.complete = false;
	}
	present[self_id] = true;
	views[self_id] = self_view | self_bit;

	if (!nodes && node_count > 0U) {
		result.complete = false;
		node_count = 0;
	}
	for (size_t i = 0; i < node_count; i++) {
		uint8_t id = nodes[i].small_id;

		if (id >= 32U || present[id]) {
			result.complete = false;
			continue;
		}
		uint32_t bit = 1U << id;

		if (!(nodes[i].view & bit)) {
			result.complete = false;
		}
		present[id] = true;
		views[id] = nodes[i].view | bit;
	}

	uint32_t known = 0;

	for (unsigned int id = 0; id < 32U; id++) {
		if (present[id]) {
			known |= 1U << id;
		}
	}

	result.component = self_bit;
	uint32_t frontier = self_bit;

	while (frontier) {
		unsigned int from = 0;

		while (from < 32U && !(frontier & (1U << from))) {
			from++;
		}
		if (from >= 32U) {
			break;
		}
		frontier &= ~(1U << from);
		if (views[from] & ~known) {
			result.complete = false;
		}

		for (unsigned int to = 0; to < 32U; to++) {
			uint32_t to_bit = 1U << to;

			if (!present[to] || (result.component & to_bit)) {
				continue;
			}
			if ((views[from] & to_bit) &&
			    (views[to] & (1U << from))) {
				result.component |= to_bit;
				frontier |= to_bit;
			}
		}
	}

	return result;
}

void group_consensus_init(struct group_consensus_policy *policy,
			  uint8_t self_id)
{
	memset(policy, 0, sizeof(*policy));
	if (self_id < 32U) {
		policy->agreed = 1U << self_id;
	}
}

uint32_t group_consensus_step(struct group_consensus_policy *policy,
			      uint8_t self_id, uint32_t target,
			      bool target_valid,
			      unsigned int agree_ticks)
{
	if (!target_valid) {
		memset(policy->run, 0, sizeof(policy->run));
		return policy->agreed;
	}
	if (self_id < 32U) {
		target |= 1U << self_id;
	}

	uint32_t next = policy->agreed;

	for (unsigned int bit = 0; bit < 32U; bit++) {
		bool want = ((target >> bit) & 1U) != 0U;
		bool have = ((policy->agreed >> bit) & 1U) != 0U;

		if (want == have) {
			policy->run[bit] = 0;
			continue;
		}

		policy->run[bit] = increment_saturated(policy->run[bit],
							agree_ticks);
		if (agree_ticks == 0U || policy->run[bit] >= agree_ticks) {
			policy->run[bit] = 0;
			next = want ? next | (1U << bit) : next & ~(1U << bit);
		}
	}

	if (self_id < 32U) {
		next |= 1U << self_id;
	}
	policy->agreed = next;
	return next;
}

void group_lock_init(struct group_lock_policy *policy)
{
	memset(policy, 0, sizeof(*policy));
}

enum group_lock_change group_lock_step(struct group_lock_policy *policy,
				       const struct group_lock_config *config,
				       bool samples_ready,
				       int64_t worst_offset_us)
{
	if (samples_ready && worst_offset_us < config->lock_on_us) {
		policy->unlit_run = 0;
		policy->lit_run = increment_saturated(policy->lit_run,
						      config->lock_ticks);
		if (policy->lit_run >= config->lock_ticks && !policy->lit) {
			policy->lit = true;
			return GROUP_LOCKED;
		}
	} else if (!samples_ready || worst_offset_us > config->lock_off_us) {
		policy->lit_run = 0;
		policy->unlit_run = increment_saturated(policy->unlit_run,
							config->unlock_ticks);
		if (policy->unlit_run >= config->unlock_ticks && policy->lit) {
			policy->lit = false;
			return GROUP_UNLOCKED;
		}
	} else {
		policy->lit_run = 0;
		policy->unlit_run = 0;
	}

	return GROUP_LOCK_NONE;
}

void group_handoff_init(struct group_handoff_policy *policy)
{
	memset(policy, 0, sizeof(*policy));
}

enum group_handoff_decision group_handoff_step(
	struct group_handoff_policy *policy, bool have_committed_group,
	bool candidate_certified, unsigned int grace_ticks)
{
	if (candidate_certified || !have_committed_group) {
		policy->uncertified_ticks = 0;
		return GROUP_HANDOFF_NONE;
	}
	if (policy->uncertified_ticks < grace_ticks) {
		policy->uncertified_ticks++;
		return GROUP_HANDOFF_HOLD_COMMITTED;
	}
	return GROUP_HANDOFF_EXPIRED;
}

float group_solo_weight(float current_group_fraction,
			float old_group_fraction)
{
	float coverage = current_group_fraction + old_group_fraction;

	if (coverage < 0.f) {
		coverage = 0.f;
	} else if (coverage > 1.f) {
		coverage = 1.f;
	}
	return 1.f - coverage;
}
