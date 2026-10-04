/* sync_engine.c — verbatim transplant of sync.c's decision logic (see
 * sync_engine.h). Any behavior change here must come with host-test
 * evidence; the firmware shim adds no decisions of its own. */

#include <stdlib.h>
#include <string.h>

#include "peer_slot_policy.h"
#include "sync_engine.h"
#include "sync_schedule.h"

void sync_engine_init(struct sync_engine *e, uint32_t my_id,
		      uint32_t my_seed, int64_t now_us)
{
	memset(e, 0, sizeof(*e));
	e->my_id = my_id;
	e->my_seed = my_seed;
	clock_estimator_init(&e->clock, now_us);
}

void sync_engine_note_sent(struct sync_engine *e, int64_t raw_us)
{
	e->sent_raw[e->sent_raw_i++ % 4U] = raw_us;
}

/* Count each fresh-window gap once whether the estimator notices it while
 * silent or the next beacon is the first observer. */
static void note_heard(struct sync_engine *e, struct sync_engine_peer *n,
		       int64_t at)
{
	if (peer_freshness_note_heard(&n->freshness, at,
				      SYNC_OVERALL_FRESH_US)) {
		e->freshness_lapses++;
	}
}

static struct sync_engine_peer *peer_slot(struct sync_engine *e,
					  uint32_t id, int64_t now)
{
	struct peer_slot_policy_entry entries[SYNC_MAX_PEERS];

	for (int i = 0; i < SYNC_MAX_PEERS; i++) {
		entries[i] = (struct peer_slot_policy_entry){
			.used = e->peers[i].used,
			.node_id = e->peers[i].id,
			.age_us = e->peers[i].used
				? peer_freshness_age_us(&e->peers[i].freshness,
							now)
				: -1,
		};
	}
	int selected = peer_slot_policy_select(entries, SYNC_MAX_PEERS, id,
					       SYNC_OVERALL_FRESH_US);

	if (selected < 0) {
		return NULL;
	}
	struct sync_engine_peer *slot = &e->peers[selected];

	if (!slot->used || slot->id != id) {
		memset(slot, 0, sizeof(*slot));
		slot->used = true;
		slot->id = id;
		slot->small_id = 0xff;
		peer_freshness_init(&slot->freshness);
		peer_freshness_init(&slot->direct_freshness);
		clock_peer_estimator_init(&slot->clock);
	}
	return slot;
}

enum sync_engine_rx sync_engine_rx_beacon(struct sync_engine *e,
					  const struct beacon_message *b,
					  int64_t arrival_us, bool direct)
{
	bool has_group_fields = b->wire_version >= 2U;

	if (b->node_id == e->my_id) {
		/* Our own multicast echo carries a raw stamp we JUST sent;
		 * a cloned node_id (FICR mis-read, copied provisioning)
		 * cannot forge one. Clones used to vanish into this filter
		 * silently while third parties watched one identity flip
		 * between two views and re-minted the scene every few
		 * seconds with nothing anywhere saying why (gap-hunt G3). */
		for (size_t i = 0; i < 4U; i++) {
			if (b->raw_us == e->sent_raw[i]) {
				return SYNC_ENGINE_RX_SELF_ECHO;
			}
		}
		e->clone_suspects++;
		return SYNC_ENGINE_RX_CLONE_SUSPECT;
	}

	struct sync_engine_peer *n = peer_slot(e, b->node_id, arrival_us);

	if (!n) {
		return SYNC_ENGINE_RX_TABLE_FULL;
	}
	if (direct) {
		(void)peer_freshness_note_heard(&n->direct_freshness,
						arrival_us,
						SYNC_DIRECT_FRESH_US);
	}
	/* duplicate of a beacon we already stamped (same sender raw clock):
	 * the second copy arrived via the slower path and its extra transit
	 * delay reads as a 20-70 ms offset jump — it was tripping the step
	 * detector into clearing the phase window (gate flapped at REST).
	 * First copy = least delay = the one the min-delay filter wants. */
	if (b->raw_us == n->last_raw) {
		note_heard(e, n, arrival_us);   /* still counts for freshness */
		return SYNC_ENGINE_RX_DUPLICATE;
	}
	n->last_raw = b->raw_us;

	note_heard(e, n, arrival_us);
	n->seed = b->seed;
	n->wire_version = b->wire_version;
	if (has_group_fields) {
		n->small_id = b->small_id;
		n->view = b->view;
		memcpy(n->ext, b->ext, 8);
	} else if (direct) {
		/* fallback: a link-local source carries the ext addr in its
		 * IID (universal/local bit flipped); ML-EID IIDs are random —
		 * left zeroed then and the group layer reads "far". The shim
		 * passes the IID-derived bytes via b->ext for v1 frames. */
		memcpy(n->ext, b->ext, 8);
	}

	clock_peer_estimator_add_sample(
		&n->clock, &clock_estimator_default_config, arrival_us,
		b->global_us, b->raw_us, 1.0 + (double)b->rate_ppb * 1e-9);
	return SYNC_ENGINE_RX_ACCEPTED;
}

void sync_engine_step(struct sync_engine *e, int64_t now_us, int64_t dt_us)
{
	struct clock_peer_input inputs[SYNC_MAX_PEERS];
	size_t input_count = 0;

	for (int i = 0; i < SYNC_MAX_PEERS; i++) {
		struct sync_engine_peer *n = &e->peers[i];

		if (!n->used) {
			continue;
		}
		int64_t age_us = peer_freshness_age_us(&n->freshness, now_us);

		if (peer_freshness_observe(&n->freshness, now_us,
					   SYNC_OVERALL_FRESH_US)) {
			e->freshness_lapses++;
		}
		if (age_us > SYNC_PEER_TIMEOUT_US) {
			n->used = false;
			continue;
		}
		inputs[input_count++] = (struct clock_peer_input){
			.peer = &n->clock,
			.fresh = peer_freshness_is_fresh(
				&n->freshness, now_us,
				SYNC_OVERALL_FRESH_US),
		};
	}
	clock_estimator_update(&e->clock, &clock_estimator_default_config,
			       inputs, input_count, now_us, dt_us);
}

double sync_engine_global_at(const struct sync_engine *e, int64_t local_us)
{
	return clock_estimator_global_at(&e->clock, local_us);
}

int sync_engine_get_peers(const struct sync_engine *e, int64_t now_us,
			  struct sync_peer *out, int max)
{
	int cnt = 0;

	for (int i = 0; i < SYNC_MAX_PEERS && cnt < max; i++) {
		const struct sync_engine_peer *n = &e->peers[i];

		if (!n->used) {
			continue;
		}
		out[cnt] = (struct sync_peer){
			.node_id = n->id,
			.seed = n->seed,
			.small_id = n->small_id,
			.view = n->view,
			.fresh = peer_freshness_is_fresh(&n->freshness, now_us,
							 SYNC_OVERALL_FRESH_US),
			.direct = peer_freshness_is_fresh(
				&n->direct_freshness, now_us,
				SYNC_DIRECT_FRESH_US),
			.age_ms = peer_freshness_age_us(&n->freshness, now_us) /
				  1000,
			.delta_us = n->clock.last_delta_us,
			.wire_legacy = n->wire_version < 3U,
		};
		memcpy(out[cnt].ext, n->ext, 8);
		cnt++;
	}
	return cnt;
}

void sync_engine_set_peer_weight(struct sync_engine *e, uint32_t node_id,
				 float w)
{
	for (int i = 0; i < SYNC_MAX_PEERS; i++) {
		if (e->peers[i].used && e->peers[i].id == node_id) {
			clock_peer_estimator_set_weight(&e->peers[i].clock, w);
			break;
		}
	}
}

void sync_engine_fill_stats(const struct sync_engine *e, int64_t now_us,
			    struct sync_stats *s)
{
	s->rate_ppm = (e->clock.h - 1.0) * 1e6;
	/* net correction vs raw uptime (near 0 free-running, moves on
	 * scramble, settles on heal) — far more legible than the epoch
	 * form of θ, which grows with uptime. */
	s->theta_us = (int64_t)(sync_engine_global_at(e, now_us) -
				(double)now_us);
	s->neighbors = 0;
	s->locked = 0;
	s->tracked = 0;
	s->oldest_age_ms = -1;
	s->freshness_lapses = e->freshness_lapses;
	s->clone_suspects = e->clone_suspects;
	s->nearest_off_us = -1;
	for (int i = 0; i < SYNC_MAX_PEERS; i++) {
		const struct sync_engine_peer *n = &e->peers[i];

		if (!n->used) {
			continue;
		}
		int64_t age_us = peer_freshness_age_us(&n->freshness, now_us);

		s->tracked++;
		if (age_us / 1000 > s->oldest_age_ms) {
			s->oldest_age_ms = age_us / 1000;
		}
		if (age_us >= SYNC_OVERALL_FRESH_US) {
			continue;
		}
		s->neighbors++;
		if (n->clock.locked) {
			s->locked++;
		}
		int64_t a = llabs((long long)n->clock.last_delta_us);

		if (s->nearest_off_us < 0 || a < s->nearest_off_us) {
			s->nearest_off_us = a;
		}
	}
}
