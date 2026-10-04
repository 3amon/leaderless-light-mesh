#include <assert.h>
#include <stdio.h>

#include "peer_freshness.h"

#define FRESH_US 2200000LL

int main(void)
{
	struct peer_freshness peer;

	peer_freshness_init(&peer);
	assert(peer_freshness_age_us(&peer, 0) == -1);
	assert(!peer_freshness_observe(&peer, 9999999, FRESH_US));
	assert(!peer_freshness_note_heard(&peer, 100, FRESH_US));
	assert(peer_freshness_is_fresh(&peer, 100 + FRESH_US - 1, FRESH_US));
	assert(!peer_freshness_observe(&peer, 100 + FRESH_US - 1, FRESH_US));

	assert(peer_freshness_observe(&peer, 100 + FRESH_US, FRESH_US));
	assert(!peer_freshness_observe(&peer, 100 + FRESH_US + 1, FRESH_US));
	assert(!peer_freshness_note_heard(&peer, 100 + FRESH_US + 2, FRESH_US));

	/* A gap first noticed by the next arrival still produces one lapse. */
	assert(peer_freshness_note_heard(&peer, 100 + 2 * FRESH_US + 2, FRESH_US));
	assert(!peer_freshness_note_heard(&peer, 100 + 2 * FRESH_US + 3, FRESH_US));

	puts("peer freshness tests passed");
	return 0;
}
