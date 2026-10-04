/* sync.c — leaderless gossip clock synchronization.
 *
 * Disciplined logical clock:   global(t) = h · (local(t) − epoch) + θ
 *   local  — raw hardware uptime, µs, never modified
 *   θ      — offset term, tuned by the PHASE loop
 *   h      — rate trim (~1.0), tuned by the FREQUENCY loop
 *   epoch  — re-anchored to "now" on every update so an h change pivots at
 *            now and never jumps the clock.
 *
 * Every node broadcasts { global, raw, rate, seed } on independently jittered
 * link-local and realm-local schedules. Each update moves theta halfway to the
 * mean neighbour offset and nudges h toward the neighbourhood mean tick rate.
 *
 * Key subtleties, learned the hard way in the sim (see TIME-SYNC.md):
 *   - min-delay filter: transit delay only ever makes an offset sample read
 *     LOW, so trust the MAX over a short window.
 *   - regress the RATE on the neighbour's RAW clock, never its logical one —
 *     a neighbour mid-correction looks like a huge frequency error otherwise.
 *   - slew small corrections, snap only big ones, deadband the tiny ones.
 */

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/openthread.h>
#include <zephyr/logging/log.h>
#include <openthread/ip6.h>
#include <openthread/link.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "beacon_codec.h"
#include "clock_estimator.h"
#include "network_mode.h"
#include "sync.h"
#include "sync_engine.h"
#include "sync_schedule.h"

LOG_MODULE_REGISTER(sync, LOG_LEVEL_INF);

_Static_assert(CLOCK_ESTIMATOR_MAX_PEERS == SYNC_MAX_PEERS,
	       "clock consensus votes must cover every sync peer");

/* ---- tuning (µs unless noted) — mirrors the combined simulation ---- */
#define GOSSIP_PORT       4243
/* Gossip rendezvous multicast. Realm-local ff03::1 is MPL-flooded mesh-wide
 * so clock and topology records converge regardless of routing shape.
 * Link-local ff02::1 is one radio hop and supplies the direct-visibility
 * evidence used by grouping. BOTH is the production default. */
#define MCAST_REALM       "ff03::1"
#define MCAST_LOCAL       "ff02::1"
#define UPDATE_MS         400

/* ---- state (all under g_mtx) ---- */
static struct k_mutex g_mtx;
/* every table/estimator decision lives in the pure engine */
static struct sync_engine g_eng;
static int64_t g_clone_warn_us;    /* LOG rate limit only */
static struct sync_compat_policy g_compat;
static uint32_t g_id, g_seed;
static uint32_t g_rng;             /* beacon-jitter LCG, seeded from node_id */
static int gsock = -1;             /* RX: a thread blocks in recvfrom on this */
static int g_txfd = -1;            /* TX: a bound, proven socket lent by the app
				    * (the heartbeat socket) via sync_set_tx_sock.
				    * (The old unbound "gtx" fallback fd never
				    * got multicast onto wpan0 and is gone.) */
/* deliberately unlocked: single word, written from shell/main, read by
 * sync_tick — a stale read costs one beacon on the old rendezvous */
static enum sync_dest_mode g_dest_mode = SYNC_DEST_BOTH;
static uint8_t  g_my_small_id = 0xff;    /* advertised in v2 beacons */
static uint32_t g_my_view;
static uint32_t g_view_generation;

/* ---- deliberate rate skew (test instrumentation) ----
 * local_us() is the hardware clock as the rest of this file perceives it.
 * A nonzero skew makes it run fast/slow like a worse crystal would; a
 * spinlock guards the doubles (torn 64-bit reads on M4 otherwise — the RX
 * thread stamps arrivals through here with no other lock held). */
static struct k_spinlock g_skew_lock;
static double  g_skew_ppm;
static double  g_skew_off_us;      /* offset accrued under previous skews */
static int64_t g_skew_anchor;      /* hw µs when g_skew_ppm last changed */

static inline int64_t hw_us(void)
{
	return k_ticks_to_us_near64((uint64_t)k_uptime_ticks());
}

static inline int64_t local_us(void)
{
	int64_t r = hw_us();
	k_spinlock_key_t key = k_spin_lock(&g_skew_lock);
	int64_t v = r + (int64_t)(g_skew_off_us +
				  (double)(r - g_skew_anchor) * g_skew_ppm * 1e-6);
	k_spin_unlock(&g_skew_lock, key);
	return v;
}

void sync_skew_set(double ppm)
{
	k_spinlock_key_t key = k_spin_lock(&g_skew_lock);
	int64_t r = hw_us();

	/* fold the old skew's accrued offset in, so a rate change never
	 * steps the clock */
	g_skew_off_us += (double)(r - g_skew_anchor) * g_skew_ppm * 1e-6;
	g_skew_anchor = r;
	g_skew_ppm = ppm;
	k_spin_unlock(&g_skew_lock, key);
	LOG_INF("clock skew set to %+d ppm", (int)ppm);
}

double sync_skew_get(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_skew_lock);
	double p = g_skew_ppm;
	k_spin_unlock(&g_skew_lock, key);
	return p;
}

/* global clock evaluated at an arbitrary local time (caller holds lock) */
static inline double global_at_locked(int64_t lu)
{
	return sync_engine_global_at(&g_eng, lu);
}

int64_t sync_global_us(void)
{
	k_mutex_lock(&g_mtx, K_FOREVER);
	struct clock_estimator clock = g_eng.clock;
	k_mutex_unlock(&g_mtx);
	return (int64_t)clock_estimator_global_at(&clock, local_us());
}

int64_t sync_global_ms(void) { return sync_global_us() / 1000; }

void sync_scramble_ms(int64_t ms)
{
	k_mutex_lock(&g_mtx, K_FOREVER);
	clock_estimator_scramble(&g_eng.clock, (double)ms * 1000.0);
	k_mutex_unlock(&g_mtx);
}

void sync_set_tx_sock(int fd)
{
	g_txfd = fd;
}

void sync_set_dest(enum sync_dest_mode mode)
{
	g_dest_mode = mode;
	LOG_INF("gossip dest = %s", sync_dest());
}

const char *sync_dest(void)
{
	switch (g_dest_mode) {
	case SYNC_DEST_LOCAL: return MCAST_LOCAL;
	case SYNC_DEST_REALM: return MCAST_REALM;
	default:              return "ff02+ff03 (both)";
	}
}

/* ---- RX: stamp arrival, hand the shim-derived facts to the engine ---- */
static void handle_beacon(const struct beacon_message *b, int64_t la,
			  const struct sockaddr_in6 *src)
{
	const uint8_t *source = src->sin6_addr.s6_addr;
	bool direct = source[0] == 0xfe && (source[1] & 0xc0) == 0x80;
	struct beacon_message copy = *b;

	if (b->wire_version < 2U && direct) {
		/* v1 frames carry no ext addr; a link-local source encodes
		 * it in the IID (universal/local bit flipped). Derive it
		 * here — addresses are the shim's business. */
		memcpy(copy.ext, source + 8, 8);
		copy.ext[0] ^= 0x02;
	}

	k_mutex_lock(&g_mtx, K_FOREVER);
	sync_compat_policy_note_wire_version(&g_compat, b->wire_version,
					     k_uptime_get());
	enum sync_engine_rx rx = sync_engine_rx_beacon(&g_eng, &copy, la,
						       direct);
	uint32_t suspects = g_eng.clone_suspects;
	k_mutex_unlock(&g_mtx);

	if (rx == SYNC_ENGINE_RX_CLONE_SUSPECT &&
	    la - g_clone_warn_us > 30000000) {
		g_clone_warn_us = la;
		LOG_WRN("beacon claims MY node id %08x with a foreign raw clock — cloned identity? (%u suspects)",
			g_eng.my_id, suspects);
	}
}

/* ---- the estimator: one phase step + one frequency step ---- */
static void estimator(int64_t dt_us)
{
	k_mutex_lock(&g_mtx, K_FOREVER);
	sync_engine_step(&g_eng, local_us(), dt_us);
	k_mutex_unlock(&g_mtx);
}

/* ---- beacon TX ---- */
static void send_beacon(enum sync_schedule_path path)
{
	int fd = g_txfd;

	if (fd < 0) {
		return;
	}
	/* our own ext addr rides every beacon (cached on first use) */
	static uint8_t my_ext[8];
	static bool have_ext;

	if (!have_ext) {
		struct openthread_context *octx = openthread_get_default_context();

		if (octx) {
			openthread_api_mutex_lock(octx);
			memcpy(my_ext,
			       otLinkGetExtendedAddress(octx->instance)->m8, 8);
			openthread_api_mutex_unlock(octx);
			have_ext = true;
		}
	}

	k_mutex_lock(&g_mtx, K_FOREVER);
	int64_t lu = local_us();

	/* remember our recent raw stamps so the RX path can tell a genuine
	 * multicast echo of this beacon from a cloned node_id */
	sync_engine_note_sent(&g_eng, lu);
	struct beacon_message b = {
		.node_id = g_id,
		.global_us = (int64_t)global_at_locked(lu),
		.raw_us = lu,
		.rate_ppb = (int32_t)((g_eng.clock.h - 1.0) * 1e9),
		.seed = g_seed,
		.small_id = g_my_small_id,
		.view = g_my_view,
	};
	memcpy(b.ext, my_ext, 8);
	k_mutex_unlock(&g_mtx);
	uint8_t wire[BEACON_WIRE_SIZE];
	/* v0.1.63 already decodes v3. Sending only v3 lets a v0.1.64 receiver
	 * treat any legacy frame as positive evidence that migration is still in
	 * progress; v0.1.64 then retains the corrected compatibility cadence. */
	int wire_size = beacon_encode(wire, sizeof(wire), &b);

	if (wire_size < 0) {
		LOG_ERR("beacon encode failed: %d", wire_size);
		return;
	}

	/* Local and realm transmissions have independent schedules. This avoids
	 * the old back-to-back pair and makes expensive MPL floods much rarer
	 * than direct-radio visibility updates. */
	static const struct { const char *addr; int hops; } dests[2] = {
		{ MCAST_LOCAL, 1 }, { MCAST_REALM, 8 },
	};
	int destination = path == SYNC_SCHEDULE_LOCAL ? 0 : 1;

	zsock_setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
			 &dests[destination].hops,
			 sizeof(dests[destination].hops));
	struct sockaddr_in6 dst = {
		.sin6_family = AF_INET6,
		.sin6_port = htons(GOSSIP_PORT),
	};

	zsock_inet_pton(AF_INET6, dests[destination].addr, &dst.sin6_addr);
	if (zsock_sendto(fd, wire, (size_t)wire_size, 0,
			 (struct sockaddr *)&dst, sizeof(dst)) < 0) {
		LOG_WRN("beacon tx (%s) failed: %d",
			dests[destination].addr, errno);
	}
}

void sync_send_aux(const uint8_t *wire, size_t length)
{
	int fd = g_txfd;
	static const struct { const char *addr; int hops; } dests[2] = {
		{ MCAST_LOCAL, 1 }, { MCAST_REALM, 8 },
	};

	if (fd < 0 || wire == NULL || length == 0) {
		return;
	}
	for (int i = 0; i < 2; i++) {
		zsock_setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
				 &dests[i].hops, sizeof(dests[i].hops));
		struct sockaddr_in6 dst = {
			.sin6_family = AF_INET6,
			.sin6_port = htons(GOSSIP_PORT),
		};

		zsock_inet_pton(AF_INET6, dests[i].addr, &dst.sin6_addr);
		if (zsock_sendto(fd, wire, length, 0,
				 (struct sockaddr *)&dst, sizeof(dst)) < 0) {
			LOG_WRN("aux gossip tx (%s) failed: %d", dests[i].addr,
				errno);
		}
	}
}

void sync_tick(bool attached)
{
	static int64_t next_local, next_realm, next_update, last_update;
	static bool was_attached;
	static enum sync_schedule_profile active_profile = SYNC_SCHEDULE_COMPAT;
	static uint32_t seen_view_generation;
	int64_t now = k_uptime_get();
	enum sync_schedule_profile profile;
	uint32_t view_generation;

	k_mutex_lock(&g_mtx, K_FOREVER);
	profile = sync_compat_policy_profile(&g_compat, now);
	view_generation = g_view_generation;
	k_mutex_unlock(&g_mtx);

	if (attached && (!was_attached || profile != active_profile)) {
		g_rng = g_rng * 1664525u + 1013904223u;
		next_local = now + sync_schedule_delay_ms(
			profile, SYNC_SCHEDULE_LOCAL, g_rng);
		g_rng = g_rng * 1664525u + 1013904223u;
		next_realm = now + sync_schedule_delay_ms(
			profile, SYNC_SCHEDULE_REALM, g_rng);
		if (profile != active_profile) {
			LOG_INF("gossip schedule = %s",
				sync_schedule_profile_name(profile));
		}
		active_profile = profile;
	}
	if (attached && view_generation != seen_view_generation) {
		g_rng = g_rng * 1664525u + 1013904223u;
		int64_t changed_realm = now +
			sync_schedule_change_delay_ms(g_rng);

		if (changed_realm < next_realm) {
			next_realm = changed_realm;
		}
		seen_view_generation = view_generation;
	}
	if (attached && now >= next_local) {
		if (g_dest_mode != SYNC_DEST_REALM) {
			send_beacon(SYNC_SCHEDULE_LOCAL);
		}
		g_rng = g_rng * 1664525u + 1013904223u;
		next_local = now + sync_schedule_delay_ms(
			profile, SYNC_SCHEDULE_LOCAL, g_rng);
	}
	if (attached && now >= next_realm) {
		if (g_dest_mode != SYNC_DEST_LOCAL) {
			send_beacon(SYNC_SCHEDULE_REALM);
		}
		g_rng = g_rng * 1664525u + 1013904223u;
		next_realm = now + sync_schedule_delay_ms(
			profile, SYNC_SCHEDULE_REALM, g_rng);
	}
	was_attached = attached;
	if (now >= next_update) {
		int64_t dt = last_update ? (now - last_update) : UPDATE_MS;

		last_update = now;
		next_update = now + UPDATE_MS;
		estimator(dt * 1000);   /* ms → µs */
	}
}

void sync_get_stats(struct sync_stats *s)
{
	k_mutex_lock(&g_mtx, K_FOREVER);
	int64_t now = local_us();

	sync_engine_fill_stats(&g_eng, now, s);
	s->skew_ppm = sync_skew_get();
	s->profile = sync_schedule_profile_name(
		sync_compat_policy_profile(&g_compat, k_uptime_get()));
	k_mutex_unlock(&g_mtx);
}

/* ---- RX thread: block on recv, stamp the instant it returns ---- */
#define RX_STACK 4096
static K_THREAD_STACK_DEFINE(rx_stack, RX_STACK);
static struct k_thread rx_thread;

static void rx_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	uint8_t wire[BEACON_WIRE_SIZE];
	struct sockaddr_in6 src;

	while (gsock < 0) {
		k_sleep(K_MSEC(100));
	}
	for (;;) {
		socklen_t sl = sizeof(src);
		int n = zsock_recvfrom(gsock, wire, sizeof(wire), 0,
				       (struct sockaddr *)&src, &sl);
		int64_t la = local_us();
		struct beacon_message message;

		if (n > 0 && network_mode_receive(wire, (size_t)n)) {
			continue;
		}
		if (n > 0 && beacon_decode(&message, wire, (size_t)n) == BEACON_CODEC_OK) {
			handle_beacon(&message, la, &src);
		}
	}
}

/* ---- peer export / group-layer hooks ---- */

int sync_get_peers(struct sync_peer *out, int max)
{
	k_mutex_lock(&g_mtx, K_FOREVER);
	int cnt = sync_engine_get_peers(&g_eng, local_us(), out, max);

	k_mutex_unlock(&g_mtx);
	return cnt;
}

void sync_set_view(uint8_t my_small_id, uint32_t view)
{
	k_mutex_lock(&g_mtx, K_FOREVER);
	if (g_my_small_id != my_small_id || g_my_view != view) {
		g_view_generation++;
	}
	g_my_small_id = my_small_id;
	g_my_view = view;
	k_mutex_unlock(&g_mtx);
}

void sync_set_peer_weight(uint32_t node_id, float w)
{
	k_mutex_lock(&g_mtx, K_FOREVER);
	sync_engine_set_peer_weight(&g_eng, node_id, w);
	k_mutex_unlock(&g_mtx);
}

void sync_init(uint32_t node_id, uint32_t seed)
{
	k_mutex_init(&g_mtx);
	g_id = node_id;
	g_seed = seed;
	g_rng = node_id ? node_id : 1;
	sync_compat_policy_init(&g_compat, k_uptime_get());
	sync_engine_init(&g_eng, node_id, seed, local_us());

	gsock = zsock_socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (gsock < 0) {
		LOG_ERR("gossip socket failed: %d", gsock);
		return;
	}

	struct sockaddr_in6 bind_addr = {
		.sin6_family = AF_INET6,
		.sin6_port = htons(GOSSIP_PORT),
		.sin6_addr = in6addr_any,
	};
	if (zsock_bind(gsock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		LOG_ERR("gossip bind failed");
	}

	/* Explicitly subscribe to realm-local all-nodes so MPL floods of our
	 * beacons are actually delivered up to this socket (link-local ff02::1
	 * is always joined; ff03::1 may not be). */
	struct openthread_context *octx = openthread_get_default_context();

	if (octx) {
		otIp6Address ma;

		if (otIp6AddressFromString(MCAST_REALM, &ma) == OT_ERROR_NONE) {
			openthread_api_mutex_lock(octx);
			otError e = otIp6SubscribeMulticastAddress(octx->instance, &ma);
			openthread_api_mutex_unlock(octx);
			LOG_INF("subscribe %s -> %d", MCAST_REALM, (int)e);
		}
	}

	k_thread_create(&rx_thread, rx_stack, RX_STACK, rx_fn,
			NULL, NULL, NULL, 6, 0, K_NO_WAIT);
	k_thread_name_set(&rx_thread, "gossip_rx");
	LOG_INF("gossip sync up: id=%08x on %s:%d", node_id, sync_dest(), GOSSIP_PORT);
}
