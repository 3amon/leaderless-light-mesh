/* groups.c — Zephyr shim around the pure group_engine decision core.
 *
 * A fresh gossip peer is locally visible only when sync received its
 * link-local ff02 beacon copy, which cannot cross a radio hop. Signal strength
 * remains telemetry, never a membership gate. Multi-hop MPL delivery therefore
 * cannot collapse the entire installation into one group, while even the
 * weakest directly heard link is allowed to join.
 *
 * ALL cohesion decisions live in group_engine.c (shared verbatim with the
 * host simulator and tests). This file only gathers inputs (uptime, the
 * sync-layer peer snapshot, OpenThread RSSI telemetry), applies outputs
 * (view/weight pushes into sync), and logs the engine's events.
 */

#include <zephyr/kernel.h>
#include <zephyr/net/openthread.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <openthread/thread.h>
#include <stddef.h>
#include <string.h>

#include "group_engine.h"
#include "groups.h"
#include "sync.h"

LOG_MODULE_REGISTER(groups, LOG_LEVEL_INF);

static struct group_engine g_engine;

/* The sid collision-repair round persists across reboots so the same
 * collision is fought once, not after every power cycle. */
#define SID_ROUND_MAGIC 0x53524e44U /* "SRND" */
#define SID_ROUND_KEY "ringgrp/sidround"

struct sid_round_store {
	uint32_t magic;
	int32_t round;
	uint32_t crc;
} __packed;

BUILD_ASSERT(sizeof(struct sid_round_store) <= SETTINGS_MAX_VAL_LEN);

static uint32_t sid_round_crc(const struct sid_round_store *value)
{
	return crc32_ieee((const uint8_t *)value,
			  offsetof(struct sid_round_store, crc));
}

static int sid_round_load_cb(const char *key, size_t len,
			     settings_read_cb read_cb, void *cb_arg,
			     void *param)
{
	struct sid_round_store *loaded = param;

	if (strcmp(key, "sidround") != 0 || len != sizeof(*loaded)) {
		return 0;
	}
	if (read_cb(cb_arg, loaded, sizeof(*loaded)) != sizeof(*loaded)) {
		memset(loaded, 0, sizeof(*loaded));
	}
	return 1;
}

static int sid_round_load(void)
{
	struct sid_round_store store = {0};

	if (settings_subsys_init() != 0) {
		return 0;
	}
	(void)settings_load_subtree_direct("ringgrp", sid_round_load_cb,
					   &store);
	if (store.magic != SID_ROUND_MAGIC ||
	    store.crc != sid_round_crc(&store) || store.round < 0) {
		return 0;
	}
	return (int)store.round;
}

static void sid_round_save(int round)
{
	struct sid_round_store store = {
		.magic = SID_ROUND_MAGIC,
		.round = (int32_t)round,
	};

	store.crc = sid_round_crc(&store);
	int rc = settings_save_one(SID_ROUND_KEY, &store, sizeof(store));

	if (rc != 0) {
		LOG_WRN("sid round persist failed: %d", rc);
	}
}

uint8_t groups_small_id(void) { return group_engine_small_id(&g_engine); }
uint32_t groups_agreed(void) { return group_engine_agreed(&g_engine); }
bool groups_lit(void) { return group_engine_lit(&g_engine); }

void groups_init(uint32_t my_node_id, uint32_t my_seed)
{
	group_engine_init(&g_engine, my_node_id, my_seed);
	int round = sid_round_load();

	if (round > 0) {
		group_engine_restore_sid_round(&g_engine, round);
		LOG_INF("restored sid repair round %d", round);
	}
	sync_set_view(g_engine.my_sid, g_engine.my_view);
	LOG_INF("group layer up: small_id=%u", g_engine.my_sid);
}

/* RSSI telemetry from the OT neighbour table when the directly heard peer is
 * also a protocol neighbour. Visibility itself comes from sync's link-local
 * beacon receipt and does not depend on this lookup succeeding. */
static bool ot_rssi_for(const uint8_t ext[8], float *rssi_out)
{
	static const uint8_t zeros[8];

	*rssi_out = GROUP_ENGINE_RSSI_UNAVAILABLE;
	if (!memcmp(ext, zeros, 8)) {
		return false;
	}
	struct openthread_context *octx = openthread_get_default_context();

	if (!octx) {
		return false;
	}
	otNeighborInfo info;
	otNeighborInfoIterator it = OT_NEIGHBOR_INFO_ITERATOR_INIT;
	bool found = false;

	openthread_api_mutex_lock(octx);
	while (otThreadGetNextNeighborInfo(octx->instance, &it, &info) ==
	       OT_ERROR_NONE) {
		if (!memcmp(info.mExtAddress.m8, ext, 8)) {
			*rssi_out = (float)info.mLastRssi;
			found = true;
			break;
		}
	}
	openthread_api_mutex_unlock(octx);
	return found;
}

void groups_tick(void)
{
	/* Static, not stack: ~2 KB of snapshot/event buffers, and this is
	 * only ever called from the main loop. When sync_peer grew by one
	 * aligned byte-turned-eight (wire_legacy) these arrays pushed the
	 * 4 KB main stack over its edge and the overflow wedged the main
	 * loop into the watchdog within seconds of the first beacons — the
	 * v0.1.82 fleet incident. Big per-tick buffers stay off the stack. */
	static struct sync_peer sp[SYNC_MAX_PEERS];
	static float rssi[SYNC_MAX_PEERS];
	static struct group_engine_events events;
	int n = sync_get_peers(sp, SYNC_MAX_PEERS);

	for (int i = 0; i < n; i++) {
		rssi[i] = GROUP_ENGINE_RSSI_UNAVAILABLE;
		if (sp[i].fresh && sp[i].direct) {
			(void)ot_rssi_for(sp[i].ext, &rssi[i]);
		}
	}

	group_engine_tick(&g_engine, k_uptime_get(), sp, n, rssi, &events);
	if (!events.ticked) {
		return;
	}

	if (events.sid_conflict) {
		LOG_WRN("sid conflict with %08x — re-derived sid=%u (round %d)",
			events.sid_conflict_with, g_engine.my_sid,
			g_engine.sid_round);
		sid_round_save(g_engine.sid_round);
	}
	for (int i = 0; i < events.join_count; i++) {
		LOG_INF("peer %08x JOINS (direct rssi %.0f)",
			events.joins[i].node_id, (double)events.joins[i].rssi);
	}
	for (int i = 0; i < events.leave_count; i++) {
		LOG_INF("peer %08x LEAVES (direct %d fresh %d)",
			events.leaves[i].node_id,
			events.leaves[i].direct ? 1 : 0,
			events.leaves[i].fresh ? 1 : 0);
	}
	for (int i = 0; i < events.retire_count; i++) {
		LOG_INF("retire departed peer %08x sid=%u (age %lld ms)",
			events.retired[i].node_id, events.retired[i].small_id,
			(long long)events.retired[i].age_ms);
	}
	if (events.lock_change == GROUP_LOCKED) {
		LOG_INF("group LOCKED (worst median %lld us)",
			(long long)g_engine.lock_worst_us);
	} else if (events.lock_change == GROUP_UNLOCKED) {
		LOG_INF("group UNLOCKED (worst median %lld us)",
			(long long)g_engine.lock_worst_us);
	}
	if (events.minted) {
		LOG_INF("group minted: seed=%08x size=%d tier=%s",
			events.mint_seed, events.mint_size,
			social_tier_name(events.mint_tier));
	} else if (events.dissolved) {
		LOG_INF("group dissolved");
	}

	sync_set_view(g_engine.my_sid, g_engine.my_view);
	for (int i = 0; i < n; i++) {
		/* A negative weight marks a peer the engine had no table
		 * slot for. That peer is a stranger by definition — leaving
		 * its sync trust at the initial 1.0 let over-cap strangers
		 * outvote discounted in-crowd peers (gap-hunt G1). */
		sync_set_peer_weight(sp[i].node_id,
				     g_engine.peer_weight[i] >= 0.f
					     ? g_engine.peer_weight[i]
					     : 0.05f);
	}
}

void groups_get_render(struct groups_render *out)
{
	struct group_engine_render render;

	group_engine_get_render(&g_engine, &render);
	out->group_w = render.group_w;
	out->group_scene = render.group_scene;
	out->old_w = render.old_w;
	out->old_scene = render.old_scene;
	out->approach_w = render.approach_w;
	out->solo_w = render.solo_w;
}

void groups_set_party(bool party)
{
	group_engine_set_party(&g_engine, party);
}

void groups_set_ultra(bool ultra)
{
	group_engine_set_ultra(&g_engine, ultra);
}

void groups_set_accent(float accent)
{
	group_engine_set_accent(&g_engine, accent);
}

void groups_set_effect(float period_s, float duty, bool fade)
{
	group_engine_set_effect(&g_engine, period_s, duty, fade);
}

int groups_group_pattern(void)
{
	return group_engine_pattern(&g_engine, sync_global_ms());
}

uint8_t groups_member_count(void)
{
	return group_engine_member_count(&g_engine);
}

enum social_tier groups_tier(void)
{
	return group_engine_tier(&g_engine);
}

int groups_status(char *buf, int len)
{
	return group_engine_status(&g_engine, buf, len);
}
