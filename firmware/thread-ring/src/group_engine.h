/* group_engine — the complete group-cohesion decision core, as pure C.
 *
 * This is groups.c's brain with the Zephyr edges cut off: no clock, no
 * radio, no OpenThread RSSI lookup, no logging. The firmware shim
 * (groups.c) feeds it the sync-layer peer snapshot plus wall time and
 * applies its outputs; the host simulator feeds it synthetic snapshots and
 * fast-forwarded time. Both compile THIS file, so a cohesion bug the
 * simulator finds is by construction the same bug the fleet ships.
 *
 * Behavior is a verbatim transplant of the v0.1.70 groups.c decision
 * logic: sid collision repair, view construction, reciprocal-topology
 * agreement, seed matching, the median phase-lock gate, bounded handoff,
 * scene minting/crossfade state, and departed-peer retirement.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "group_policy.h"
#include "social_scene.h"
#include "sync.h"

#define GROUP_ENGINE_TICK_MS          1000
#define GROUP_ENGINE_RSSI_UNAVAILABLE (-128.f)

struct group_engine_peer {
	bool     used;
	uint32_t node_id;
	uint32_t seed;
	uint8_t  small_id;
	uint32_t view;
	bool     direct;         /* fresh link-local beacon copy received */
	bool     heard_fresh;    /* sync-layer freshness (45 s window). Gates
				  * timing-median validity; NOT the same as
				  * presence.fresh, which since the G2 fix
				  * holds the 15 s "hearing" signal that
				  * discriminates silence from visible loss.
				  * Coupling the two unlocks the whole crowd
				  * 15 s into any member's silence. */
	float    rssi_s;         /* telemetry only; unavailable when multi-hop */
	struct group_peer_presence presence;
	int64_t  delta_us;
	/* recent |delta| history — the phase-lock gate reads the MEDIAN */
	int64_t  dhist[5];
	int      dhist_n, dhist_i;
	/* Timing demotion: a member whose median stays over the lock bound
	 * is dropped from MY VIEW (not from membership) so the healthy
	 * majority re-mints without it instead of the old behavior where
	 * one bad clock held the whole crowd dark. Hysteretic both ways. */
	unsigned int timing_bad_run, timing_good_run;
	bool     timing_demoted;
	/* (Deliberately NO chronic-demotion escalation: demotion already
	 * excludes a bad-clock member from views, which both stabilizes the
	 * healthy scene and — by breaking reciprocity — drops the faulty
	 * node to honest solo. Expelling it would only add a rejoin/demote
	 * re-mint cycle; verified in tests/c/campaign/integrated.c I6.) */
	/* EMA of "are we hearing this peer" over ~2 minutes. A member dark
	 * the vast majority of the window is a ghost: it keeps scene
	 * identity it almost never renders. Members below the floor are
	 * expelled and re-earn membership when their link stabilizes. */
	float    presence_avg;
};

/* One tick's noteworthy transitions, for the shim to log and the simulator
 * to assert on. Counts saturate at the array sizes. */
struct group_engine_events {
	bool ticked;             /* false: called again inside the same tick */
	bool sid_conflict;
	uint32_t sid_conflict_with;
	enum group_lock_change lock_change;
	bool minted;
	bool dissolved;
	uint32_t mint_seed;
	int mint_size;
	enum social_tier mint_tier;
	uint8_t join_count, leave_count, retire_count;
	struct { uint32_t node_id; float rssi; } joins[SYNC_MAX_PEERS];
	struct { uint32_t node_id; bool direct; bool fresh; } leaves[SYNC_MAX_PEERS];
	struct { uint32_t node_id; uint8_t small_id; int64_t age_ms; } retired[SYNC_MAX_PEERS];
};

struct group_engine {
	struct group_engine_peer peers[SYNC_MAX_PEERS];
	uint32_t my_id, my_seed;
	uint8_t  my_sid;
	uint32_t my_view;        /* my member bits (incl. self) */
	int      sid_round;
	struct group_consensus_policy consensus;
	struct group_topology_result topology;
	struct group_lock_policy lock;
	struct group_handoff_policy handoff;
	int64_t  last_tick_ms;

	/* Crossfade state: current social scene + fading previous scene. */
	struct social_scene group_scene, old_scene;
	bool     have_group, have_old;
	float    fade;           /* 0..1 progress of current layer fade-in */
	float    old_fade;       /* fading-out weight of previous scene */
	float    lock_visibility;/* 0..1 render gate with bounded fades */
	uint32_t group_seed_cur;
	uint32_t group_members_cur;
	uint32_t lock_measured_members;
	uint32_t lock_timed_members;
	int64_t  lock_worst_us;

	/* Per-input-peer sync trust weights computed this tick; parallel to
	 * the tick's input array. The shim forwards them to
	 * sync_set_peer_weight; the simulator can assert on them. */
	float    peer_weight[SYNC_MAX_PEERS];
};

void group_engine_init(struct group_engine *e, uint32_t my_node_id,
		       uint32_t my_seed);

/* Restore a persisted sid collision-repair round after boot (the round is
 * kept in settings so the same collision is not re-fought after every
 * reboot). Re-derives the small id exactly like a live conflict does. */
void group_engine_restore_sid_round(struct group_engine *e, int round);

/* One ~1 Hz cohesion step. `sp` is the sync-layer peer snapshot; `rssi`
 * is a parallel array of radio RSSI readings for directly heard peers
 * (GROUP_ENGINE_RSSI_UNAVAILABLE when the lookup failed or does not
 * apply) — pass NULL when no telemetry exists at all. After a tick,
 * peer_weight[i] < 0 marks an input peer that found no table slot; such a
 * peer is a stranger and the shim applies the stranger trust weight
 * (0.05) rather than leaving whatever weight the sync layer initialized.
 * Returns through `events`;
 * when events->ticked is false the call was inside the current tick
 * window and nothing changed. */
void group_engine_tick(struct group_engine *e, int64_t now_ms,
		       const struct sync_peer *sp, int n, const float *rssi,
		       struct group_engine_events *events);

/* Pure render-plan math over engine state (weights include fades). */
struct group_engine_render {
	float group_w;
	struct social_scene group_scene;
	float old_w;
	struct social_scene old_scene;
	float approach_w;
	float solo_w;
};
void group_engine_get_render(const struct group_engine *e,
			     struct group_engine_render *out);

void group_engine_set_party(struct group_engine *e, bool party);
void group_engine_set_ultra(struct group_engine *e, bool ultra);
void group_engine_set_accent(struct group_engine *e, float accent);
void group_engine_set_effect(struct group_engine *e, float period_s,
			     float duty, bool fade);
int group_engine_status(const struct group_engine *e, char *buf, int len);
int group_engine_pattern(const struct group_engine *e, int64_t global_ms);
uint8_t group_engine_member_count(const struct group_engine *e);
enum social_tier group_engine_tier(const struct group_engine *e);

static inline uint8_t group_engine_small_id(const struct group_engine *e)
{
	return e->my_sid;
}

static inline uint32_t group_engine_agreed(const struct group_engine *e)
{
	return e->consensus.agreed;
}

static inline bool group_engine_lit(const struct group_engine *e)
{
	return e->lock.lit && e->have_group;
}
