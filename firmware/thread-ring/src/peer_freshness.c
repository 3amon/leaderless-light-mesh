#include "peer_freshness.h"

void peer_freshness_init(struct peer_freshness *peer)
{
	peer->last_heard_us = -1;
	peer->lapsed = false;
}

int64_t peer_freshness_age_us(const struct peer_freshness *peer, int64_t now_us)
{
	return peer->last_heard_us < 0 ? -1 : now_us - peer->last_heard_us;
}

bool peer_freshness_is_fresh(const struct peer_freshness *peer, int64_t now_us,
			     int64_t fresh_window_us)
{
	int64_t age_us = peer_freshness_age_us(peer, now_us);

	return age_us >= 0 && age_us < fresh_window_us;
}

bool peer_freshness_observe(struct peer_freshness *peer, int64_t now_us,
			    int64_t fresh_window_us)
{
	if (peer->lapsed || peer_freshness_is_fresh(peer, now_us, fresh_window_us) ||
	    peer->last_heard_us < 0) {
		return false;
	}
	peer->lapsed = true;
	return true;
}

bool peer_freshness_note_heard(struct peer_freshness *peer, int64_t now_us,
			       int64_t fresh_window_us)
{
	bool new_lapse = peer_freshness_observe(peer, now_us, fresh_window_us);

	peer->last_heard_us = now_us;
	peer->lapsed = false;
	return new_lapse;
}
