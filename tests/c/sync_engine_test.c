/* Host tests for the extracted sync_engine decision core. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "sync_engine.h"
#include "sync_schedule.h"

static struct beacon_message beacon(uint32_t id, int64_t raw, int64_t global)
{
	struct beacon_message b;

	memset(&b, 0, sizeof(b));
	b.wire_version = 3;
	b.node_id = id;
	b.raw_us = raw;
	b.global_us = global;
	b.seed = id ^ 0x5eedU;
	b.small_id = (uint8_t)(id % 32U);
	b.view = 1U << (id % 32U);
	return b;
}

static void test_accept_dedupe_and_snapshot(void)
{
	struct sync_engine e;

	sync_engine_init(&e, 0x100, 0xabc, 0);
	struct beacon_message b = beacon(0x200, 5000, 1005000);

	assert(sync_engine_rx_beacon(&e, &b, 10000, true) ==
	       SYNC_ENGINE_RX_ACCEPTED);
	/* identical raw stamp = slower multicast copy: freshness only, no
	 * second clock sample (it would trip the step detector) */
	assert(sync_engine_rx_beacon(&e, &b, 60000, false) ==
	       SYNC_ENGINE_RX_DUPLICATE);
	b = beacon(0x200, 1500000, 2500000);
	assert(sync_engine_rx_beacon(&e, &b, 1510000, false) ==
	       SYNC_ENGINE_RX_ACCEPTED);

	struct sync_peer out[SYNC_MAX_PEERS];
	int n = sync_engine_get_peers(&e, 1600000, out, SYNC_MAX_PEERS);

	assert(n == 1);
	assert(out[0].node_id == 0x200);
	assert(out[0].fresh);
	assert(out[0].direct);        /* within the 12 s direct window */
	assert(!out[0].wire_legacy);  /* v3 wire */
	assert(out[0].small_id == 0x200 % 32U);
}

static void test_self_echo_vs_clone(void)
{
	struct sync_engine e;

	sync_engine_init(&e, 0x100, 0xabc, 0);
	sync_engine_note_sent(&e, 777777);
	struct beacon_message b = beacon(0x100, 777777, 1000000);

	assert(sync_engine_rx_beacon(&e, &b, 780000, true) ==
	       SYNC_ENGINE_RX_SELF_ECHO);
	assert(e.clone_suspects == 0);
	/* same node id with a raw stamp we never sent = clone */
	b.raw_us = 123456;
	assert(sync_engine_rx_beacon(&e, &b, 800000, true) ==
	       SYNC_ENGINE_RX_CLONE_SUSPECT);
	assert(e.clone_suspects == 1);
	/* neither may occupy a table slot */
	struct sync_peer out[SYNC_MAX_PEERS];

	assert(sync_engine_get_peers(&e, 900000, out, SYNC_MAX_PEERS) == 0);
}

static void test_timeout_eviction_and_lapse(void)
{
	struct sync_engine e;

	sync_engine_init(&e, 0x100, 0xabc, 0);
	struct beacon_message b = beacon(0x200, 5000, 5000);

	assert(sync_engine_rx_beacon(&e, &b, 10000, true) ==
	       SYNC_ENGINE_RX_ACCEPTED);
	/* silent past the fresh window: one lapse, still tracked */
	sync_engine_step(&e, SYNC_OVERALL_FRESH_US + 1000000, 400000);
	assert(e.freshness_lapses == 1);
	struct sync_peer out[SYNC_MAX_PEERS];

	assert(sync_engine_get_peers(&e, SYNC_OVERALL_FRESH_US + 1000000, out,
				     SYNC_MAX_PEERS) == 1);
	assert(!out[0].fresh);
	/* silent past retention: slot released */
	sync_engine_step(&e, SYNC_PEER_TIMEOUT_US + 2000000, 400000);
	assert(sync_engine_get_peers(&e, SYNC_PEER_TIMEOUT_US + 2000000, out,
				     SYNC_MAX_PEERS) == 0);
}

static void test_legacy_wire_flag_and_weight(void)
{
	struct sync_engine e;

	sync_engine_init(&e, 0x100, 0xabc, 0);
	struct beacon_message b = beacon(0x300, 4000, 4000);

	b.wire_version = 1;
	assert(sync_engine_rx_beacon(&e, &b, 9000, true) ==
	       SYNC_ENGINE_RX_ACCEPTED);
	struct sync_peer out[SYNC_MAX_PEERS];

	assert(sync_engine_get_peers(&e, 20000, out, SYNC_MAX_PEERS) == 1);
	assert(out[0].wire_legacy);
	/* weight application reaches the per-peer estimator */
	sync_engine_set_peer_weight(&e, 0x300, 0.1f);
	assert(e.peers[0].clock.weight > 0.09f &&
	       e.peers[0].clock.weight < 0.11f);
}

static void test_stats_derivation(void)
{
	struct sync_engine e;

	sync_engine_init(&e, 0x100, 0xabc, 0);
	struct beacon_message b = beacon(0x200, 5000, 5000);

	assert(sync_engine_rx_beacon(&e, &b, 10000, true) ==
	       SYNC_ENGINE_RX_ACCEPTED);
	struct sync_stats s;

	memset(&s, 0, sizeof(s));
	sync_engine_fill_stats(&e, 20000, &s);
	assert(s.tracked == 1);
	assert(s.neighbors == 1);
	assert(s.clone_suspects == 0);
	assert(s.oldest_age_ms >= 0);
}

int main(void)
{
	test_accept_dedupe_and_snapshot();
	test_self_echo_vs_clone();
	test_timeout_eviction_and_lapse();
	test_legacy_wire_flag_and_weight();
	test_stats_derivation();
	printf("sync engine tests passed\n");
	return 0;
}
