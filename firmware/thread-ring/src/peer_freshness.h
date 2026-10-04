/* Pure peer-freshness transition policy; no Zephyr dependencies. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct peer_freshness {
	int64_t last_heard_us;
	bool lapsed;
};

void peer_freshness_init(struct peer_freshness *peer);
bool peer_freshness_note_heard(struct peer_freshness *peer, int64_t now_us,
			       int64_t fresh_window_us);
bool peer_freshness_observe(struct peer_freshness *peer, int64_t now_us,
			    int64_t fresh_window_us);
int64_t peer_freshness_age_us(const struct peer_freshness *peer, int64_t now_us);
bool peer_freshness_is_fresh(const struct peer_freshness *peer, int64_t now_us,
			     int64_t fresh_window_us);
