/* sync_engine — the sync layer's decision core, as pure C.
 *
 * This is sync.c's brain with the Zephyr edges cut off: no sockets, no
 * mutex, no logging, no uptime source. The firmware shim (sync.c) feeds
 * it decoded beacons with arrival stamps and applies its outputs; host
 * tests and simulators feed it synthetic traffic. Both compile THIS
 * file, so a table/dedupe/trust bug a simulator finds is by construction
 * the same bug the fleet ships — the same contract group_engine and
 * clock_estimator already honor.
 *
 * Owned here: the neighbour table (slot lifecycle via peer_slot_policy,
 * freshness windows, timeout eviction), beacon acceptance (self-echo vs
 * cloned-identity classification, duplicate-raw dedupe feeding the
 * min-delay filter), per-peer clock sample ingestion, the disciplined
 * clock estimator step, peer-snapshot/stat derivation, and trust-weight
 * application. Left to the shim: sockets and addresses (the DIRECT bit
 * is derived from the source address), schedules, the compat policy,
 * skew instrumentation, and logging.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "beacon_codec.h"
#include "clock_estimator.h"
#include "peer_freshness.h"
#include "sync.h"

struct sync_engine_peer {
	uint32_t id;
	bool     used;
	struct peer_freshness freshness;
	uint32_t seed;
	uint8_t  small_id;     /* 0xff until a v2 beacon arrives */
	uint32_t view;
	uint8_t  ext[8];       /* ext addr from beacon or link-local IID */
	struct peer_freshness direct_freshness; /* ff02 radio-hop copies */
	int64_t  last_raw;     /* raw stamp of the last accepted beacon */
	struct clock_peer_estimator clock;
	uint8_t  wire_version; /* of the last accepted beacon */
};

struct sync_engine {
	uint32_t my_id, my_seed;
	struct sync_engine_peer peers[SYNC_MAX_PEERS];
	struct clock_estimator clock;
	/* clone-identity detection: raw stamps of our recent sends — a
	 * genuine multicast echo matches one exactly, a clone cannot */
	int64_t  sent_raw[4];
	unsigned int sent_raw_i;
	uint32_t clone_suspects;
	uint32_t freshness_lapses;
};

enum sync_engine_rx {
	SYNC_ENGINE_RX_ACCEPTED,      /* new sample folded in */
	SYNC_ENGINE_RX_DUPLICATE,     /* slower copy: freshness only */
	SYNC_ENGINE_RX_SELF_ECHO,     /* our own multicast loopback */
	SYNC_ENGINE_RX_CLONE_SUSPECT, /* OUR node id, foreign raw clock */
	SYNC_ENGINE_RX_TABLE_FULL,    /* no slot won by this sender */
};

void sync_engine_init(struct sync_engine *e, uint32_t my_id,
		      uint32_t my_seed, int64_t now_us);

/* Record the raw stamp of a beacon we are about to transmit, so its
 * multicast echo is recognized as ours. */
void sync_engine_note_sent(struct sync_engine *e, int64_t raw_us);

/* Fold one received beacon in. `arrival_us` is the local stamp taken the
 * instant recv returned; `direct` means the source address was
 * link-local (one radio hop). */
enum sync_engine_rx sync_engine_rx_beacon(struct sync_engine *e,
					  const struct beacon_message *b,
					  int64_t arrival_us, bool direct);

/* One estimator step: evict timed-out peers, count freshness lapses,
 * and advance the disciplined clock by dt_us. */
void sync_engine_step(struct sync_engine *e, int64_t now_us, int64_t dt_us);

double sync_engine_global_at(const struct sync_engine *e, int64_t local_us);

int sync_engine_get_peers(const struct sync_engine *e, int64_t now_us,
			  struct sync_peer *out, int max);

void sync_engine_set_peer_weight(struct sync_engine *e, uint32_t node_id,
				 float w);

/* Fills every table- and clock-derived field of `s`. The shim adds what
 * only it knows: skew instrumentation and the schedule profile. */
void sync_engine_fill_stats(const struct sync_engine *e, int64_t now_us,
			    struct sync_stats *s);
