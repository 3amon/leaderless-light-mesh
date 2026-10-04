/* sync.h — leaderless gossip clock synchronization (GTSP-style).
 *
 * The engine from TIME-SYNC.md, in firmware: every node keeps a disciplined
 * logical clock global = h·(local − epoch) + θ, broadcasts it to radio
 * neighbours over link-local multicast, and moves halfway to the neighbourhood
 * mean each update. No master, no election. See sync.c for the algorithm.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Call once after the net stack is up. node_id/seed identify this board
 * (derive from FICR). Creates the gossip socket and starts the RX thread. */
void sync_init(uint32_t node_id, uint32_t seed);

/* Call frequently from the main loop; internally rate-limited. `attached`
 * gates beacon TX (only transmit when on-channel in the Thread network). */
void sync_tick(bool attached);

/* The disciplined global clock. Patterns and flash mode run on this. */
int64_t sync_global_us(void);
int64_t sync_global_ms(void);

/* Test disturbance: shove the logical clock by `ms`. The engine heals it. */
void sync_scramble_ms(int64_t ms);

/* Test disturbance: bias the perceived hardware clock rate by `ppm`,
 * simulating a worse crystal — beacons, discipline and rendering all see the
 * skewed clock, so the frequency loop must learn it out. 0 restores the true
 * rate; the offset accrued while skewed is kept (rate change, no phase step). */
void sync_skew_set(double ppm);
double sync_skew_get(void);

/* Route beacon TX through an existing, working socket (the heartbeat socket,
 * bound to :4242). An UNBOUND send-only socket silently failed multicast
 * egress on this stack — heartbeats flowed, beacons never hit wpan0. Beacon
 * sends set IPV6_MULTICAST_HOPS per-send, so callers sharing the socket must
 * set their own hops before each send too. */
void sync_set_tx_sock(int fd);

/* Send a small auxiliary gossip frame to both scopes. Ordinary v3 clock
 * beacons remain unchanged so rolling upgrades stay wire-compatible. */
void sync_send_aux(const uint8_t *wire, size_t length);

/* Beacon rendezvous multicast mode. BOTH sends independently scheduled
 * ff02::1 link-local visibility beacons and slower ff03::1 realm floods. */
enum sync_dest_mode { SYNC_DEST_LOCAL, SYNC_DEST_REALM, SYNC_DEST_BOTH };
void sync_set_dest(enum sync_dest_mode mode);
const char *sync_dest(void);

/* ---- peer export for the pattern/group layer ---- */
/* A fleet/group is intentionally capped at 24 nodes: self plus 23 peers.
 * The bitmap keeps 25 collision-resolved IDs so SID collision repair still
 * has one spare value, but only 24 nodes participate at once. */
#define SYNC_MAX_TOTAL_NODES 24
#define SYNC_MAX_PEERS (SYNC_MAX_TOTAL_NODES - 1)

struct sync_peer {
	uint32_t node_id;
	uint32_t seed;
	uint8_t  small_id;     /* 5-bit flash-time id from beacon v2; 0xff = v1 */
	uint32_t view;         /* sender's membership bitmap (beacon v2) */
	bool     fresh;        /* heard within the fresh window */
	bool     direct;       /* a link-local copy was heard within the fresh
				* window; independent of signal strength */
	int64_t  age_ms;       /* time since the most recent beacon copy */
	int64_t  delta_us;     /* last measured offset to them (signed) */
	bool     wire_legacy;  /* last beacon was pre-v3 wire: firmware old
				* enough to predate the estimator fixes, so
				* its clock never earns full trust */
	uint8_t  ext[8];       /* 802.15.4 ext addr derived from the beacon's
				* link-local source IID; all-zero when the
				* source wasn't IID-derived (e.g. ML-EID) */
};

/* Snapshot the neighbour table (fresh and stale-but-tracked). Returns count. */
int sync_get_peers(struct sync_peer *out, int max);

/* Membership view + small id this node advertises in its own beacons. */
void sync_set_view(uint8_t my_small_id, uint32_t view);

/* Trust-ladder phase weight for a neighbour (TIME-SYNC.md tuning lesson #1):
 * members 1.0, approaching candidates graded, strangers a whisper. Weights
 * are normalized in the estimator, so a sole neighbour converges at full
 * speed regardless. Unset defaults to 1.0. */
void sync_set_peer_weight(uint32_t node_id, float w);

struct sync_stats {
	double  rate_ppm;        /* (h − 1) · 1e6 — learned frequency trim */
	double  skew_ppm;        /* deliberate test rate bias (sync_skew_set) */
	int64_t theta_us;        /* current offset term */
	int     neighbors;       /* neighbours heard within the fresh window */
	int     locked;          /* of those, how many agree within the lock gate */
	int     tracked;         /* fresh + stale peers retained until timeout */
	int64_t oldest_age_ms;   /* oldest retained peer, -1 when none */
	uint32_t freshness_lapses; /* fresh -> stale transitions since boot */
	uint32_t clone_suspects; /* beacons claiming OUR node id with a
				  * raw clock we never sent — a cloned
				  * identity somewhere in radio range */
	int64_t nearest_off_us;  /* |offset| to the best-agreeing neighbour */
	const char *profile;     /* compat during rolling migration, else scalable */
};
void sync_get_stats(struct sync_stats *s);
