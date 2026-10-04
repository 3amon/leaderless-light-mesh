/* Pure group presence, mutual topology, consensus, and phase-lock policy.
 *
 * This module deliberately has no Zephyr, radio, clock, or rendering
 * dependencies so its state transitions can be replayed in host CI.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum group_member_state {
	GROUP_STRANGER,
	GROUP_CANDIDATE,
	GROUP_MEMBER,
};

enum group_membership_change {
	GROUP_MEMBERSHIP_NONE,
	GROUP_MEMBERSHIP_JOINED,
	GROUP_MEMBERSHIP_LEFT,
};

struct group_membership_policy {
	enum group_member_state state;
	unsigned int run;
};

struct group_membership_config {
	unsigned int join_ticks;
	unsigned int leave_ticks;
	unsigned int grace_ticks;
};

void group_membership_init(struct group_membership_policy *policy);
enum group_membership_change group_membership_step(
	struct group_membership_policy *policy,
	const struct group_membership_config *config, bool visible, bool fresh);

/* Presence is deliberately separate from the sync table's lifetime. A peer
 * must continue advancing through departure grace after the clock layer has
 * discarded its stale sample record. */
struct group_peer_presence {
	struct group_membership_policy membership;
	bool visible;
	bool fresh;
	int64_t age_ms;
};

void group_peer_presence_init(struct group_peer_presence *presence);
enum group_membership_change group_peer_presence_step(
	struct group_peer_presence *presence,
	const struct group_membership_config *config, bool observed,
	bool observed_visible, bool observed_fresh, int64_t observed_age_ms,
	int64_t elapsed_ms);

/* The sync layer owns the 90-second observation lifetime. Once it no longer
 * exports a peer and membership grace has also released it, no old consensus
 * bit may keep that peer record alive: doing so can deadlock topology after a
 * small-ID collision is resolved. */
bool group_peer_retire_ready(const struct group_peer_presence *presence,
			     bool observed);

/* Each beacon carries its sender's local, hysteretic proximity view. Build a
 * deterministic component from reciprocal edges: i--j exists only when both
 * advertised views contain the other endpoint. `complete` is false when a
 * component member refers to an unknown ID, an ID is duplicated, or a view is
 * malformed; incomplete topology is never render-lock evidence. */
struct group_topology_node {
	uint8_t small_id;
	uint32_t view;
};

struct group_topology_result {
	uint32_t component;
	bool complete;
};

struct group_topology_result group_topology_component(
	uint8_t self_id, uint32_t self_view,
	const struct group_topology_node *nodes, size_t node_count);

struct group_consensus_policy {
	uint32_t agreed;
	unsigned int run[32];
};

void group_consensus_init(struct group_consensus_policy *policy,
			  uint8_t self_id);
uint32_t group_consensus_step(struct group_consensus_policy *policy,
			      uint8_t self_id, uint32_t target,
			      bool target_valid,
			      unsigned int agree_ticks);

enum group_lock_change {
	GROUP_LOCK_NONE,
	GROUP_LOCKED,
	GROUP_UNLOCKED,
};

struct group_lock_policy {
	bool lit;
	unsigned int lit_run;
	unsigned int unlit_run;
};

struct group_lock_config {
	int64_t lock_on_us;
	int64_t lock_off_us;
	unsigned int lock_ticks;
	unsigned int unlock_ticks;
};

/* Production visual-lock hysteresis. Hardware camera tests showed that
 * 20-70 ms beacon timestamp excursions do not represent equivalent render
 * skew; acquisition uses the measured 80 ms level. Release sits 20% above
 * it (96 ms) so a crowd whose worst median breathes across the bar holds
 * its scene instead of flapping lit at tick rate — the dusty-lotus live
 * incident (2026-08-17). Time hysteresis remains asymmetric: acquire in
 * three ticks, release in five. */
extern const struct group_lock_config group_render_lock_config;

void group_lock_init(struct group_lock_policy *policy);
enum group_lock_change group_lock_step(struct group_lock_policy *policy,
				       const struct group_lock_config *config,
				       bool samples_ready,
				       int64_t worst_offset_us);

/* A certified group is the committed visual state. Brief topology/agreement
 * mismatch while another membership set converges must not expose every
 * member's solo scene, but malformed or permanently incomplete topology must
 * still fail closed after a bounded grace interval. */
enum group_handoff_decision {
	GROUP_HANDOFF_NONE,
	GROUP_HANDOFF_HOLD_COMMITTED,
	GROUP_HANDOFF_EXPIRED,
};

struct group_handoff_policy {
	unsigned int uncertified_ticks;
};

void group_handoff_init(struct group_handoff_policy *policy);
enum group_handoff_decision group_handoff_step(
	struct group_handoff_policy *policy, bool have_committed_group,
	bool candidate_certified, unsigned int grace_ticks);

/* Solo is the complement of total group-layer coverage. During a direct
 * group-to-group crossfade the old and new fractions add to one, so solo must
 * remain hidden instead of bleeding through at the midpoint. */
float group_solo_weight(float current_group_fraction,
			float old_group_fraction);
