/* group_engine.c — pure transplant of the groups.c decision core (v0.1.70).
 *
 * See group_engine.h. Any behavior change here must come with simulator
 * evidence AND updated host tests; the firmware shim adds no decisions of
 * its own.
 */

#include <stdio.h>
#include <string.h>

#include "group_engine.h"

/* ---- tuning (verbatim from groups.c) ---- */
#define RSSI_ALPHA       0.25f      /* EMA per tick — slewed, never stepped */
#define JOIN_TICKS       20         /* deliberate ~20 s radio contact */
#define LEAVE_TICKS      20         /* reject transient direct-beacon loss */
#define GRACE_TICKS      45         /* silent member: long benefit of doubt */
#define AGREE_TICKS      5          /* topology must remain stable to flip */
#define HANDOFF_TICKS    60         /* keep the old group through convergence */
#define DEMOTE_TICKS     10         /* solid-bad ticks before demotion */
/* Demotion pressure is LEAKY, not consecutive: +2 per bad tick, -1 per
 * non-bad tick, demote at DEMOTE_TICKS*2. A solidly bad clock still
 * demotes in exactly DEMOTE_TICKS, but an OSCILLATING one — dusty-lotus
 * hovered its median across the 80 ms bar for half an hour and never
 * strung 10 consecutive bad ticks — now accumulates pressure whenever
 * it is bad more than a third of the time. */
#define DEMOTE_PRESSURE_MAX (DEMOTE_TICKS * 2U)
#define READMIT_TICKS    5          /* consecutive good ticks to readmit */
/* Readmission demands genuinely-good timing (half the lit bar), not
 * merely dipping back under it: an oscillator that briefly crosses under
 * 80 ms must not re-enter the view it will gate again seconds later. */
#define READMIT_LEVEL_US 40000
/* "Hearing" is reception of ANYTHING recently (direct or multi-hop). It is
 * the discriminator between a member that wandered out of direct range but
 * is demonstrably alive (LEAVE_TICKS applies) and one that has gone silent
 * (GRACE_TICKS applies). Without it, silence spends its first 33 ticks
 * looking like visible loss — the sync freshness window is 45 s — so the
 * leave counter always fired at ~31 s and the documented grace was
 * unreachable (gap-hunt G2). Grace now really covers a ~50 s reboot. */
#define HEARING_MS       15000
/* Ghost-membership gate (gap-hunt G6): EMA of actual reception duty
 * ("heard within one local-beacon cadence") over a ~2 minute horizon;
 * members below the floor (dark >~75% of the window) are expelled. A
 * healthy direct member beacons every second or two and scores ~0.9
 * even through heavy loss; a flapper up a few seconds a minute scores
 * ~0.1-0.2. The wider hearing/freshness windows must NOT feed this
 * gate — they exist to tolerate gaps and would credit a 3 s burst with
 * 15+ s of presence. presence_avg restarts at 1.0 on every join, so a
 * returning node gets a full window of good faith before the gate can
 * re-fire. */
#define PRESENT_NOW_MS   3000
#define PRESENCE_ALPHA   (1.f / 120.f)
#define PRESENCE_FLOOR   0.25f
#define FADE_S           3.0f       /* group crossfade */
#define LOCK_FADE_IN_S   3.0f       /* certified group appears gently */
#define LOCK_FADE_OUT_S  2.0f       /* invalid group yields quickly */
#define GROUP_W          0.85f

static const struct group_membership_config membership_config = {
	.join_ticks = JOIN_TICKS,
	.leave_ticks = LEAVE_TICKS,
	.grace_ticks = GRACE_TICKS,
};

_Static_assert(SYNC_MAX_PEERS + 1U == 24U,
	       "fleet/group hard cap must remain 24 nodes");

static uint32_t hash32(uint32_t x)
{
	x ^= x >> 16; x *= 0x7feb352dU;
	x ^= x >> 15; x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

/* small ids are hash-derived, so collisions happen (it hit our FIRST two
 * boards: both landed on 13). Resolution is deterministic: on a conflict
 * the HIGHER node_id re-derives with a bumped salt round; the lower keeps
 * its id. Both sides converge without coordination.
 *
 * The space is the full 32-bit view bitmap: with the 24-node hard cap that
 * leaves at least eight free values, so repair converges in few rounds
 * even near capacity (the old %25 left a single spare and let excluded
 * over-capacity nodes churn indefinitely). Mixed fleets are fine — sids
 * only need to be distinct, not derived identically. */
static uint8_t sid_for(uint32_t node_id, int round)
{
	return (uint8_t)(hash32(node_id ^ 0x1D5U ^ ((uint32_t)round << 8)) % 32);
}

/* Localized completeness: when several records claim one small id (a rogue
 * squatter, or the ordinary transient while collision repair converges),
 * only the claimant that repair says KEEPS the id — the lowest node_id —
 * is visible to topology and seed selection. The old behavior fed both
 * records into the snapshot, marked it incomplete, and froze agreement
 * for every node in range: one squatter with a high node_id could hold
 * the whole crowd dark forever (reproduced in cohesion_sim). Quarantined
 * records keep their membership/timing state and return the moment the
 * duplicate resolves. Claims on MY sid are quarantined unconditionally:
 * the conflict scan has already re-derived if I was the yielder, so a
 * remaining claimant is either transient or hostile. */
static bool peer_is_canonical(const struct group_engine *e,
			      const struct group_engine_peer *p)
{
	if (p->small_id == 0xff) {
		return false;
	}
	if (p->small_id == e->my_sid) {
		return false;
	}
	for (int i = 0; i < SYNC_MAX_PEERS; i++) {
		const struct group_engine_peer *q = &e->peers[i];

		if (q != p && q->used && q->small_id == p->small_id &&
		    q->node_id < p->node_id) {
			return false;
		}
	}
	return true;
}

void group_engine_init(struct group_engine *e, uint32_t my_node_id,
		       uint32_t my_seed)
{
	memset(e, 0, sizeof(*e));
	e->my_id = my_node_id;
	e->my_seed = my_seed;
	e->my_sid = sid_for(my_node_id, 0);
	e->my_view = 1u << e->my_sid;
	group_consensus_init(&e->consensus, e->my_sid);
	e->topology = group_topology_component(e->my_sid, e->my_view, NULL, 0);
	group_lock_init(&e->lock);
	group_handoff_init(&e->handoff);
}

void group_engine_restore_sid_round(struct group_engine *e, int round)
{
	if (round <= 0 || round == e->sid_round) {
		return;
	}
	e->sid_round = round;
	e->my_sid = sid_for(e->my_id, e->sid_round);
	e->my_view = 1u << e->my_sid;
	group_consensus_init(&e->consensus, e->my_sid);
	e->topology = group_topology_component(e->my_sid, e->my_view, NULL, 0);
	group_lock_init(&e->lock);
	group_handoff_init(&e->handoff);
}

static struct group_engine_peer *peer_slot(struct group_engine *e,
					   uint32_t node_id)
{
	struct group_engine_peer *free_slot = NULL;

	for (int i = 0; i < SYNC_MAX_PEERS; i++) {
		if (e->peers[i].used && e->peers[i].node_id == node_id) {
			return &e->peers[i];
		}
		if (!e->peers[i].used && !free_slot) {
			free_slot = &e->peers[i];
		}
	}
	if (free_slot) {
		memset(free_slot, 0, sizeof(*free_slot));
		free_slot->used = true;
		free_slot->node_id = node_id;
		free_slot->rssi_s = GROUP_ENGINE_RSSI_UNAVAILABLE;
		free_slot->small_id = 0xff;
		free_slot->presence_avg = 1.f;
		group_peer_presence_init(&free_slot->presence);
	}
	return free_slot;
}

/* Recompute the deterministic reciprocal-proximity component. All nodes that
 * have the same view snapshot derive the same component; incomplete snapshots
 * freeze agreement and can never unlock the render layer. */
static void update_agreement(struct group_engine *e)
{
	struct group_topology_node nodes[SYNC_MAX_PEERS];
	size_t node_count = 0;

	for (int i = 0; i < SYNC_MAX_PEERS; i++) {
		struct group_engine_peer *p = &e->peers[i];

		if (p->used && peer_is_canonical(e, p)) {
			nodes[node_count++] = (struct group_topology_node){
				.small_id = p->small_id,
				.view = p->view,
			};
		}
	}

	e->topology = group_topology_component(e->my_sid, e->my_view, nodes,
					       node_count);
	group_consensus_step(&e->consensus, e->my_sid, e->topology.component,
			     e->topology.complete, AGREE_TICKS);
}

/* Group seed = XOR of agreed members' seed-hashes (self included). Refuse a
 * partial set: a bitmap with an unknown or duplicate seed must not silently
 * mint a different scene. */
static bool agreed_group_seed(const struct group_engine *e, uint32_t *seed_out,
			      int *size_out, uint32_t *members_out)
{
	uint32_t gs = 0;
	uint32_t members = 0;
	int size = 0;
	bool complete = true;

	for (int b = 0; b < 32; b++) {
		if (!((e->consensus.agreed >> b) & 1)) {
			continue;
		}
		uint32_t seed = 0;

		if (b == e->my_sid) {
			seed = e->my_seed;
		} else {
			/* the canonical (lowest node_id) claimant owns the
			 * bit; quarantined duplicates never pick the seed */
			bool found = false;

			for (int i = 0; i < SYNC_MAX_PEERS; i++) {
				const struct group_engine_peer *p =
					&e->peers[i];

				if (p->used && p->small_id == b &&
				    peer_is_canonical(e, p)) {
					seed = p->seed;
					found = true;
					break;
				}
			}
			if (!found || !seed) {
				complete = false;
				continue;
			}
		}
		gs ^= hash32(seed);
		members |= 1U << b;
		size++;
	}
	*seed_out = gs;
	*size_out = size;
	*members_out = members;
	return complete;
}

void group_engine_tick(struct group_engine *e, int64_t now_ms,
		       const struct sync_peer *sp, int n, const float *rssi,
		       struct group_engine_events *events)
{
	memset(events, 0, sizeof(*events));

	if (n > SYNC_MAX_PEERS) {
		n = SYNC_MAX_PEERS;   /* peer_weight[] is sized to the cap */
	}
	if (now_ms - e->last_tick_ms < GROUP_ENGINE_TICK_MS) {
		return;
	}
	int64_t elapsed_ms =
		e->last_tick_ms ? now_ms - e->last_tick_ms : GROUP_ENGINE_TICK_MS;
	float dt = (float)elapsed_ms / 1000.f;

	e->last_tick_ms = now_ms;
	events->ticked = true;

	uint32_t seen = 0;

	for (int i = 0; i < n && i < SYNC_MAX_PEERS; i++) {
		e->peer_weight[i] = -1.f;   /* no slot: leave sync trust alone */
	}

	/* Exactly one direct radio neighbour and nobody agreed: the pair
	 * case for the bootstrap rule below. Multi-hop peers do not count;
	 * they can never become members anyway. Consensus is last tick's,
	 * which only delays the rule by one tick. */
	int fresh_direct = 0;

	for (int i = 0; i < n; i++) {
		if (sp[i].fresh && sp[i].direct) {
			fresh_direct++;
		}
	}
	bool alone_together = fresh_direct == 1 &&
			      e->consensus.agreed == (1u << e->my_sid);

	/* sid conflict? the higher node_id yields and re-derives */
	for (int i = 0; i < n; i++) {
		if (sp[i].fresh && sp[i].small_id == e->my_sid &&
		    sp[i].node_id != e->my_id && e->my_id > sp[i].node_id) {
			e->sid_round++;
			e->my_sid = sid_for(e->my_id, e->sid_round);
			group_consensus_init(&e->consensus, e->my_sid);
			group_lock_init(&e->lock);
			group_handoff_init(&e->handoff);
			events->sid_conflict = true;
			events->sid_conflict_with = sp[i].node_id;
			break;
		}
	}

	for (int i = 0; i < n; i++) {
		struct group_engine_peer *p = peer_slot(e, sp[i].node_id);

		if (!p) {
			continue;
		}
		seen |= 1u << (p - e->peers);
		p->seed = sp[i].seed;
		p->small_id = sp[i].small_id;
		p->view = sp[i].view;
		p->delta_us = sp[i].delta_us;
		if (sp[i].fresh) {
			int64_t a = p->delta_us < 0 ? -p->delta_us : p->delta_us;

			p->dhist[p->dhist_i] = a;
			p->dhist_i = (p->dhist_i + 1) % 5;
			if (p->dhist_n < 5) {
				p->dhist_n++;
			}
		}

		float radio_rssi =
			rssi ? rssi[i] : GROUP_ENGINE_RSSI_UNAVAILABLE;

		p->direct = sp[i].fresh && sp[i].direct;
		bool have_rssi =
			p->direct && radio_rssi > GROUP_ENGINE_RSSI_UNAVAILABLE;

		if (have_rssi) {
			if (p->rssi_s <= GROUP_ENGINE_RSSI_UNAVAILABLE) {
				p->rssi_s = radio_rssi;
			} else {
				p->rssi_s +=
					RSSI_ALPHA * (radio_rssi - p->rssi_s);
			}
		} else {
			p->rssi_s = GROUP_ENGINE_RSSI_UNAVAILABLE;
		}

		/* Any fresh direct-radio link is eligible, independent of RSSI. */
		bool hearing = sp[i].fresh && sp[i].age_ms < HEARING_MS;
		bool present_now = sp[i].fresh && sp[i].age_ms < PRESENT_NOW_MS;

		p->heard_fresh = sp[i].fresh;
		p->presence_avg += PRESENCE_ALPHA *
			((present_now ? 1.f : 0.f) - p->presence_avg);
		enum group_membership_change membership_change =
			group_peer_presence_step(&p->presence, &membership_config,
						 true, p->direct, hearing,
						 sp[i].age_ms, elapsed_ms);

		if (membership_change == GROUP_MEMBERSHIP_JOINED &&
		    events->join_count < SYNC_MAX_PEERS) {
			p->presence_avg = 1.f;
			events->joins[events->join_count].node_id = p->node_id;
			events->joins[events->join_count].rssi = p->rssi_s;
			events->join_count++;
		} else if (membership_change == GROUP_MEMBERSHIP_LEFT) {
			if (events->leave_count < SYNC_MAX_PEERS) {
				events->leaves[events->leave_count].node_id =
					p->node_id;
				events->leaves[events->leave_count].direct =
					p->direct;
				events->leaves[events->leave_count].fresh =
					p->presence.fresh;
				events->leave_count++;
			}
			p->rssi_s = GROUP_ENGINE_RSSI_UNAVAILABLE;
		}

		/* Trust rises with sustained direct contact, never signal
		 * strength — and FULL clock trust additionally requires the
		 * reciprocal agreement the scene layer already demands.
		 * Membership alone is one-way visibility, which a rogue that
		 * never reciprocates earns in 20 ticks; when membership was
		 * enough for weight 1.0, liars needed only half the MEMBERS
		 * to capture the crowd clock while every scene stayed lit
		 * (gap-hunt G1). Agreement/topology lag one tick here, which
		 * costs a joining honest node a few ticks at 0.6. */
		float progress = membership_config.join_ticks > 0U
			? (float)p->presence.membership.run /
				  (float)membership_config.join_ticks
			: 1.f;
		bool has_sid = p->small_id != 0xff;
		bool agreed_bit = has_sid &&
			((e->consensus.agreed >> p->small_id) & 1U);
		bool reciprocal = has_sid &&
			((e->topology.component >> p->small_id) & 1U);
		float w = p->presence.membership.state == GROUP_MEMBER ?
			  (agreed_bit ? 1.0f : reciprocal ? 0.6f : 0.1f) :
			  p->presence.membership.state == GROUP_CANDIDATE ?
			  0.1f + 0.5f * progress :
			  0.05f;

		/* Pair bootstrap. The clock snap needs a total vote weight of
		 * 1.0, which a crowd reaches by summing several members, but
		 * two rings alone together have exactly one voice each and it
		 * only weighs 1.0 once agreed. Agreement needs the partner's
		 * timing within the lit bar, timing needs the snap: a pair
		 * whose logical clocks are far apart (one ran for hours, the
		 * other just rebooted) demote each other and never lock. Seen
		 * live 2026-09-29: sleepy-mirage-wave + tender-jelly-ember,
		 * 19,000 s apart, closing at the slew cap, ~58 h to converge.
		 *
		 * Fenced to the pair: exactly one fresh direct peer in the
		 * table, it has earned membership (20 ticks of direct contact)
		 * and I have no agreed set yet. A crowd never takes this path,
		 * so a single stranger's voice still cannot capture a node
		 * that has anyone else to listen to. The snap moves halfway,
		 * on both sides, so mutual snapping converges rather than
		 * oscillating. */
		if (alone_together && p->direct &&
		    p->presence.membership.state == GROUP_MEMBER) {
			w = 1.0f;
		}

		/* A pre-v3-wire peer runs firmware older than the estimator
		 * fixes (median consensus, rate-trim); it may join scenes
		 * but its clock stays near-voiceless — a single legacy
		 * straggler (dusty-lotus) measurably dragged eight modern
		 * nodes' thetas around at weight 1.0. */
		if (sp[i].wire_legacy && w > 0.1f) {
			w = 0.1f;
		}
		e->peer_weight[i] = w;
	}

	/* The sync table eventually forgets silent peers. Continue the presence
	 * policy for every retained-but-unobserved peer so a powered-off node can
	 * actually leave rather than freezing as a permanent member. */
	for (int i = 0; i < SYNC_MAX_PEERS; i++) {
		struct group_engine_peer *p = &e->peers[i];

		if (!p->used || (seen & (1u << i))) {
			continue;
		}
		p->direct = false;
		p->heard_fresh = false;
		p->presence_avg += PRESENCE_ALPHA * (0.f - p->presence_avg);
		enum group_membership_change membership_change =
			group_peer_presence_step(&p->presence, &membership_config,
						 false, false, false, 0,
						 elapsed_ms);

		if (membership_change == GROUP_MEMBERSHIP_LEFT) {
			if (events->leave_count < SYNC_MAX_PEERS) {
				events->leaves[events->leave_count].node_id =
					p->node_id;
				events->leaves[events->leave_count].direct = false;
				events->leaves[events->leave_count].fresh = false;
				events->leave_count++;
			}
			p->rssi_s = GROUP_ENGINE_RSSI_UNAVAILABLE;
		}
	}

	/* ---- per-peer timing medians drive both the lock gate and the
	 * demotion policy, so compute them before the view is built ---- */
	int64_t median[SYNC_MAX_PEERS];
	bool have_median[SYNC_MAX_PEERS];

	for (int i = 0; i < SYNC_MAX_PEERS; i++) {
		struct group_engine_peer *p = &e->peers[i];

		have_median[i] = false;
		median[i] = 0;
		if (!p->used || !p->heard_fresh || p->dhist_n <= 0) {
			continue;
		}
		int64_t s[5];

		memcpy(s, p->dhist, sizeof(s));
		for (int a = 1; a < p->dhist_n; a++) {
			for (int b = a; b > 0 && s[b] < s[b - 1]; b--) {
				int64_t tmp = s[b];

				s[b] = s[b - 1];
				s[b - 1] = tmp;
			}
		}
		median[i] = s[p->dhist_n / 2];
		have_median[i] = true;

		/* Demotion hysteresis runs for members whether or not they
		 * are currently agreed, so a demoted peer whose clock heals
		 * can earn its way back in. */
		if (p->presence.membership.state != GROUP_MEMBER) {
			p->timing_bad_run = 0;
			p->timing_good_run = 0;
			/* A peer that has left re-enters with a clean timing
			 * slate. Carrying the flag across departure meant a
			 * partner that rebooted (clock back to zero) came back
			 * already excluded from the view, so reciprocity, and
			 * with it any snap, could never happen. Readmission
			 * only runs for members, so the stale flag was
			 * permanent. */
			p->timing_demoted = false;
			continue;
		}
		if (median[i] > group_render_lock_config.lock_on_us) {
			/* bad: worse than the lit bar */
			p->timing_good_run = 0;
			p->timing_bad_run += 2U;
			if (p->timing_bad_run > DEMOTE_PRESSURE_MAX) {
				p->timing_bad_run = DEMOTE_PRESSURE_MAX;
			}
			if (p->timing_bad_run >= DEMOTE_PRESSURE_MAX) {
				p->timing_demoted = true;
			}
		} else {
			if (p->timing_bad_run > 0) {
				p->timing_bad_run--;   /* leak, don't reset */
			}
			if (median[i] < READMIT_LEVEL_US) {
				if (p->timing_good_run < READMIT_TICKS) {
					p->timing_good_run++;
				}
				if (p->timing_demoted &&
				    p->timing_good_run >= READMIT_TICKS) {
					p->timing_demoted = false;
					p->timing_bad_run = 0;
				}
			} else {
				p->timing_good_run = 0;
			}
		}
	}

	/* ---- ghost expulsion (gap-hunt G6): a member dark the vast
	 * majority of the presence window keeps scene identity it almost
	 * never renders. Drop it back to a from-scratch candidate; the
	 * 20-tick consecutive-contact candidacy means a true flapper
	 * cannot churn its way back in, while a node whose link stabilizes
	 * re-earns membership normally. ---- */
	for (int i = 0; i < SYNC_MAX_PEERS; i++) {
		struct group_engine_peer *p = &e->peers[i];

		if (!p->used ||
		    p->presence.membership.state != GROUP_MEMBER ||
		    p->presence_avg >= PRESENCE_FLOOR) {
			continue;
		}
		if (events->leave_count < SYNC_MAX_PEERS) {
			events->leaves[events->leave_count].node_id = p->node_id;
			events->leaves[events->leave_count].direct = p->direct;
			events->leaves[events->leave_count].fresh =
				p->presence.fresh;
			events->leave_count++;
		}
		group_peer_presence_init(&p->presence);
		p->timing_demoted = false;
		p->timing_bad_run = 0;
		p->timing_good_run = 0;
		p->rssi_s = GROUP_ENGINE_RSSI_UNAVAILABLE;
	}

	/* ---- my view = self + members (minus timing-demoted peers) ---- */
	uint32_t view = 1u << e->my_sid;

	for (int i = 0; i < SYNC_MAX_PEERS; i++) {
		if (e->peers[i].used &&
		    e->peers[i].presence.membership.state == GROUP_MEMBER &&
		    !e->peers[i].timing_demoted &&
		    e->peers[i].small_id != 0xff &&
		    e->peers[i].small_id != e->my_sid) {   /* unresolved conflict */
			view |= 1u << e->peers[i].small_id;
		}
	}
	e->my_view = view;

	/* ---- consensus + minting ---- */
	update_agreement(e);

	int gsize = 0;
	uint32_t group_members = 0;
	uint32_t gseed = 0;
	bool seeds_ready =
		agreed_group_seed(e, &gseed, &gsize, &group_members);
	bool want_group = seeds_ready && gsize >= 2;
	bool topology_ready = e->topology.complete &&
		e->topology.component == e->consensus.agreed;
	bool set_certified = seeds_ready && topology_ready;

	/* phase-lock gate over agreed members. Two layers of flap-proofing:
	 * per-peer MEDIAN of the last 5 deltas (single MPL-jitter excursions
	 * vanish), and 3 consecutive ticks sustained before changing state,
	 * on top of an 80 ms level gate with asymmetric 3/5-tick time
	 * hysteresis. */
	int64_t worst = 0;
	uint32_t measured = 0;

	for (int i = 0; i < SYNC_MAX_PEERS; i++) {
		struct group_engine_peer *p = &e->peers[i];

		if (p->used && p->small_id != 0xff &&
		    ((e->consensus.agreed >> p->small_id) & 1) &&
		    have_median[i]) {
			if (median[i] > worst) {
				worst = median[i];
			}
			measured |= 1U << p->small_id;
		}
	}
	uint32_t timed_members = group_members & ~(1U << e->my_sid);
	bool scene_certified = want_group && set_certified;
	bool samples_ready = scene_certified &&
		(measured & timed_members) == timed_members;
	bool pending_timing_ready = samples_ready &&
		worst < group_render_lock_config.lock_on_us;
	bool committed_matches_set = e->have_group && set_certified &&
		group_members == e->group_members_cur;
	bool handoff_needed = e->have_group && e->lock.lit &&
		!committed_matches_set;
	bool current_blend_settled = !e->have_old && e->fade >= 1.f;
	bool next_set_ready = set_certified &&
		(!want_group || pending_timing_ready) &&
		(!handoff_needed || current_blend_settled);

	e->lock_measured_members = measured & timed_members;
	e->lock_timed_members = timed_members;
	e->lock_worst_us = worst;
	enum group_lock_change lock_change = GROUP_LOCK_NONE;

	/* A membership change first moves topology, then damped agreement. Keep a
	 * lit committed scene through that ordinary bounded interval so a joining
	 * singleton or merging cohort cannot knock every incumbent back to solo.
	 * A persistently incomplete/malformed target still expires and fails
	 * closed. */
	enum group_handoff_decision handoff = group_handoff_step(
		&e->handoff, handoff_needed, next_set_ready,
		HANDOFF_TICKS);
	bool hold_committed = handoff == GROUP_HANDOFF_HOLD_COMMITTED;

	if (hold_committed) {
		lock_change = GROUP_LOCK_NONE;
	} else if (!scene_certified) {
		if (e->lock.lit) {
			lock_change = GROUP_UNLOCKED;
		}
		group_lock_init(&e->lock);
	} else {
		lock_change = group_lock_step(&e->lock,
					      &group_render_lock_config,
					      samples_ready, worst);
	}
	events->lock_change = lock_change;

	float visibility_step =
		dt / (e->lock.lit ? LOCK_FADE_IN_S : LOCK_FADE_OUT_S);

	if (e->lock.lit) {
		e->lock_visibility += visibility_step;
		if (e->lock_visibility > 1.f) {
			e->lock_visibility = 1.f;
		}
	} else {
		e->lock_visibility -= visibility_step;
		if (e->lock_visibility < 0.f) {
			e->lock_visibility = 0.f;
		}
	}

	/* ---- crossfades: any change of (want_group, gseed) rotates the
	 * current social scene into the fading-out slot ---- */
	bool scene_commit_ready = set_certified &&
		(!e->lock.lit ||
		 ((!want_group || pending_timing_ready) && current_blend_settled));

	if (scene_commit_ready && want_group &&
	    (!e->have_group || gseed != e->group_seed_cur ||
	     group_members != e->group_members_cur)) {
		if (e->have_group) {
			e->old_scene = e->group_scene;
			e->old_fade = e->fade;
			e->have_old = true;
		}
		enum social_tier tier =
			social_tier_for_member_count((uint8_t)gsize);

		social_scene_init(&e->group_scene, gseed, tier,
				  (uint8_t)gsize);
		e->group_seed_cur = gseed;
		e->group_members_cur = group_members;
		e->have_group = true;
		e->fade = 0.f;
		events->minted = true;
		events->mint_seed = gseed;
		events->mint_size = gsize;
		events->mint_tier = tier;
	} else if (scene_commit_ready && !want_group && e->have_group) {
		e->old_scene = e->group_scene;
		e->old_fade = e->fade;
		e->have_old = true;
		e->have_group = false;
		e->group_members_cur = 0;
		events->dissolved = true;
	}

	/* advance fades (tick-rate, ~1 Hz) */
	float step = dt / FADE_S;

	if (e->have_group && e->fade < 1.f) {
		e->fade = e->fade + step > 1.f ? 1.f : e->fade + step;
	}
	if (e->have_old) {
		e->old_fade -= step;
		if (e->old_fade <= 0.f) {
			e->have_old = false;
			e->old_fade = 0.f;
		}
	}

	/* Release a departed peer once sync retention AND membership grace have
	 * both let go, even if the OLD agreed bitmap still contains its bit
	 * (waiting on agreement here is circular after collision repair). */
	for (int i = 0; i < SYNC_MAX_PEERS; i++) {
		struct group_engine_peer *p = &e->peers[i];

		if (!p->used ||
		    !group_peer_retire_ready(&p->presence,
					     (seen & (1u << i)) != 0U)) {
			continue;
		}
		if (events->retire_count < SYNC_MAX_PEERS) {
			events->retired[events->retire_count].node_id =
				p->node_id;
			events->retired[events->retire_count].small_id =
				p->small_id;
			events->retired[events->retire_count].age_ms =
				p->presence.age_ms;
			events->retire_count++;
		}
		memset(p, 0, sizeof(*p));
	}
}

void group_engine_get_render(const struct group_engine *e,
			     struct group_engine_render *out)
{
	memset(out, 0, sizeof(*out));

	float lit = e->lock_visibility;

	if (e->have_group) {
		out->group_w = GROUP_W * e->fade * lit;
		out->group_scene = e->group_scene;
	}
	if (e->have_old) {
		out->old_w = GROUP_W * e->old_fade * lit;
		out->old_scene = e->old_scene;
	}

	/* Candidates add only a subtle luminance cue ahead of membership. */
	for (int i = 0; i < SYNC_MAX_PEERS; i++) {
		const struct group_engine_peer *p = &e->peers[i];

		if (!p->used || !p->presence.fresh ||
		    p->presence.membership.state == GROUP_MEMBER) {
			continue;
		}
		float progress = membership_config.join_ticks > 0U
			? (float)p->presence.membership.run /
				  (float)membership_config.join_ticks
			: 1.f;

		if (progress <= 0.02f) {
			continue;
		}
		if (progress > out->approach_w) {
			out->approach_w = progress;
		}
	}

	float current_fraction = e->have_group ? e->fade * lit : 0.f;
	float old_fraction = e->have_old ? e->old_fade * lit : 0.f;

	out->solo_w = group_solo_weight(current_fraction, old_fraction);
}

void group_engine_set_party(struct group_engine *e, bool party)
{
	if (e->have_group) {
		social_scene_set_party(&e->group_scene, party);
	}
	if (e->have_old) {
		social_scene_set_party(&e->old_scene, party);
	}
}

void group_engine_set_ultra(struct group_engine *e, bool ultra)
{
	if (e->have_group) {
		social_scene_set_ultra(&e->group_scene, ultra);
	}
	if (e->have_old) {
		social_scene_set_ultra(&e->old_scene, ultra);
	}
}

void group_engine_set_accent(struct group_engine *e, float accent)
{
	if (e->have_group) {
		social_scene_set_accent(&e->group_scene, accent);
	}
	if (e->have_old) {
		social_scene_set_accent(&e->old_scene, accent);
	}
}

void group_engine_set_effect(struct group_engine *e, float period_s,
			     float duty, bool fade)
{
	if (e->have_group) {
		social_scene_set_effect(&e->group_scene, period_s, duty, fade);
	}
	if (e->have_old) {
		social_scene_set_effect(&e->old_scene, period_s, duty, fade);
	}
}

int group_engine_pattern(const struct group_engine *e, int64_t global_ms)
{
	return (e->have_group && e->lock.lit)
		? social_scene_pattern_id(
			  (struct social_scene *)&e->group_scene,
			  (float)global_ms / 1000.f)
		: -1;
}

uint8_t group_engine_member_count(const struct group_engine *e)
{
	return (e->have_group && e->lock.lit) ? e->group_scene.member_count : 1U;
}

enum social_tier group_engine_tier(const struct group_engine *e)
{
	return (e->have_group && e->lock.lit) ? e->group_scene.tier : SOCIAL_SOLO;
}

int group_engine_status(const struct group_engine *e, char *buf, int len)
{
	unsigned int peer_count = 0;

	for (int i = 0; i < SYNC_MAX_PEERS; i++) {
		if (e->peers[i].used) {
			peer_count++;
		}
	}
	int off = snprintf(
		buf, len,
		"sid=%u view=%08x target=%08x topo=%d agreed=%08x commit=%08x hand=%u lit=%d vis=%.2f fade=%.2f size=%u tier=%s clk=%u/%u off=%lldms peers=%u",
		e->my_sid, e->my_view, e->topology.component,
		e->topology.complete ? 1 : 0, e->consensus.agreed,
		e->have_group ? e->group_members_cur : 0,
		e->handoff.uncertified_ticks, e->lock.lit,
		(double)e->lock_visibility, (double)e->fade,
		group_engine_member_count(e),
		social_tier_name(group_engine_tier(e)),
		(unsigned int)__builtin_popcount(e->lock_measured_members),
		(unsigned int)__builtin_popcount(e->lock_timed_members),
		(long long)(e->lock_worst_us / 1000), peer_count);

	for (int i = 0; i < SYNC_MAX_PEERS && off >= 0 && off < len; i++) {
		const struct group_engine_peer *p = &e->peers[i];
		char detail[96];

		if (!p->used) {
			continue;
		}
		int written = snprintf(
			detail, sizeof(detail), " |%08x/%u/%s/d%d/f%d/a%lld/r%.0f",
			p->node_id, p->small_id,
			p->presence.membership.state == GROUP_MEMBER ? "MEM" :
			p->presence.membership.state == GROUP_CANDIDATE ? "seen" :
									 "far",
			p->direct ? 1 : 0, p->presence.fresh ? 1 : 0,
			(long long)p->presence.age_ms, (double)p->rssi_s);

		if (written < 0 || written >= (int)sizeof(detail) ||
		    written >= len - off) {
			break;
		}
		memcpy(buf + off, detail, (size_t)written + 1U);
		off += written;
	}
	return off;
}
