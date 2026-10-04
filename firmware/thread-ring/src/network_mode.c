#include "network_mode.h"

#include <stddef.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include "sync.h"

LOG_MODULE_REGISTER(network_mode, LOG_LEVEL_INF);

#define MODE_MAGIC 0x4d4f4445U /* "MODE" */
#define MODE_STORE_VERSION 3
#define MODE_STORE_KEY "fleetmode/state"
#define MODE_GOSSIP_PERIOD_MS 45000
#define MODE_GOSSIP_BURST_COUNT 3

struct mode_store {
	uint32_t magic;
	uint8_t version;
	uint8_t mode;
	uint8_t blink_cs;
	uint8_t duty_pct;
	uint8_t wave;
	/* Store v3 repurposes the old spare byte as the party accent. In a
	 * v1/v2 record this byte is a zero `reserved`, which means "no
	 * accent field", NOT "accent off" — those load as FULL. */
	uint8_t accent_pct;
	uint32_t priority;
	uint32_t crc;
} __packed;

/* The 16-byte record every build through v0.1.77 wrote (store version 1).
 * It must stay loadable forever: rejecting it on size means the whole
 * fleet forgets its mode register on the first power cycle after an OTA. */
struct mode_store_legacy {
	uint32_t magic;
	uint8_t version;
	uint8_t mode;
	uint8_t blink_cs;
	uint8_t duty_pct;
	uint32_t priority;
	uint32_t crc;
} __packed;

BUILD_ASSERT(sizeof(struct mode_store) <= SETTINGS_MAX_VAL_LEN);
BUILD_ASSERT(sizeof(struct mode_store_legacy) == 16);
BUILD_ASSERT(sizeof(struct mode_store) == 18);

static struct k_mutex g_mode_mutex;
static struct network_mode_value g_mode = {
	.mode = NETWORK_MODE_NORMAL,
	.priority = 0,
	/* Not 0: a fresh register must render the historical full-strength
	 * accent, and 0 is the real "accent off" setting. */
	.accent_pct = NETWORK_MODE_ACCENT_FULL_PCT,
	.accent_known = true,
};
static bool g_ready;
static uint32_t g_node_id;
static uint32_t g_generation;
static uint32_t g_tx_generation;
static int64_t g_next_tx_ms;
static uint8_t g_burst_remaining;
static bool g_was_attached;

static uint32_t store_crc(const struct mode_store *value)
{
	return crc32_ieee((const uint8_t *)value,
			  offsetof(struct mode_store, crc));
}

static bool store_valid(const struct mode_store *value)
{
	/* v0.1.78/79 wrote this exact 18-byte layout but still stamped
	 * version 1 (the version was never bumped with the size — the
	 * original C3 bug). Both stamps are therefore the same layout and
	 * must stay loadable, or the 79->80 upgrade re-creates the very
	 * register loss the migration exists to prevent. */
	bool accent_ok = value->version == MODE_STORE_VERSION
		? network_mode_accent_valid(value->accent_pct)
		: value->accent_pct == 0;   /* the old `reserved` byte */

	return value->magic == MODE_MAGIC &&
	       (value->version == MODE_STORE_VERSION ||
		value->version == 2U || value->version == 1U) &&
	       accent_ok &&
	       network_mode_blink_valid(value->blink_cs) &&
	       network_mode_duty_valid(value->duty_pct) &&
	       network_mode_wave_valid(value->wave) &&
	       network_mode_valid((enum network_mode)value->mode) &&
	       value->crc == store_crc(value);
}

static bool legacy_valid(const struct mode_store_legacy *value)
{
	return value->magic == MODE_MAGIC && value->version == 1U &&
	       network_mode_blink_valid(value->blink_cs) &&
	       network_mode_duty_valid(value->duty_pct) &&
	       network_mode_valid((enum network_mode)value->mode) &&
	       value->crc == crc32_ieee((const uint8_t *)value,
					offsetof(struct mode_store_legacy,
						 crc));
}

struct mode_load {
	uint8_t raw[sizeof(struct mode_store)];
	size_t len;
};

static int load_cb(const char *key, size_t len, settings_read_cb read_cb,
		   void *cb_arg, void *param)
{
	struct mode_load *loaded = param;

	if (strcmp(key, "state") != 0 ||
	    (len != sizeof(struct mode_store) &&
	     len != sizeof(struct mode_store_legacy))) {
		return 0;
	}
	if (read_cb(cb_arg, loaded->raw, len) != (ssize_t)len) {
		loaded->len = 0;
		return 1;
	}
	loaded->len = len;
	return 1;
}

int network_mode_init(uint32_t node_id)
{
	k_mutex_init(&g_mode_mutex);
	g_node_id = node_id;
	g_mode = (struct network_mode_value){
		.mode = NETWORK_MODE_NORMAL,
		.priority = 0,
		.accent_pct = NETWORK_MODE_ACCENT_FULL_PCT,
		.accent_known = true,
	};
	g_ready = false;
	int rc = settings_subsys_init();

	if (rc != 0) {
		LOG_ERR("settings init for fleet mode failed: %d", rc);
		return rc;
	}
	struct mode_load loaded = { .len = 0 };

	rc = settings_load_subtree_direct("fleetmode", load_cb, &loaded);
	if (rc == 0 && loaded.len == sizeof(struct mode_store)) {
		struct mode_store record;

		memcpy(&record, loaded.raw, sizeof(record));
		if (store_valid(&record)) {
			g_mode.mode = (enum network_mode)record.mode;
			g_mode.priority = record.priority;
			g_mode.ultra_blink_cs = record.blink_cs;
			g_mode.ultra_duty_pct = record.duty_pct;
			g_mode.wave = record.wave;
			/* Pre-v3 stores predate the accent entirely and
			 * rendered the full swell; only a v3 record can
			 * actually mean "accent off". */
			g_mode.accent_pct =
				record.version == MODE_STORE_VERSION
					? record.accent_pct
					: (uint8_t)NETWORK_MODE_ACCENT_FULL_PCT;
			g_mode.duty_known = true;
			g_mode.wave_known = true;
			g_mode.accent_known = true;
		}
	} else if (rc == 0 && loaded.len == sizeof(struct mode_store_legacy)) {
		struct mode_store_legacy record;

		memcpy(&record, loaded.raw, sizeof(record));
		if (legacy_valid(&record)) {
			/* v0.1.77-and-earlier record: no wave byte existed,
			 * and every era rendered the hard square. */
			g_mode.mode = (enum network_mode)record.mode;
			g_mode.priority = record.priority;
			g_mode.ultra_blink_cs = record.blink_cs;
			g_mode.ultra_duty_pct = record.duty_pct;
			g_mode.wave = NETWORK_MODE_WAVE_HARD;
			g_mode.accent_pct =
				(uint8_t)NETWORK_MODE_ACCENT_FULL_PCT;
			g_mode.duty_known = true;
			g_mode.wave_known = true;
			g_mode.accent_known = true;
			LOG_INF("migrated legacy fleet-mode record");
		}
	}
	g_ready = rc == 0;
	g_generation = 1;
	g_tx_generation = 0;
	g_next_tx_ms = 0;
	g_burst_remaining = 0;
	g_was_attached = false;
	LOG_INF("fleet mode %s priority %u", network_mode_name(g_mode.mode),
		g_mode.priority);
	return rc;
}

struct network_mode_value network_mode_get(void)
{
	k_mutex_lock(&g_mode_mutex, K_FOREVER);
	struct network_mode_value value = g_mode;
	k_mutex_unlock(&g_mode_mutex);
	return value;
}

static enum network_mode_apply_result apply_value(
	const struct network_mode_value *value)
{
	struct network_mode_value candidate = *value;

	if (!network_mode_valid(candidate.mode) ||
	    !network_mode_blink_valid(candidate.ultra_blink_cs) ||
	    !network_mode_duty_valid(candidate.ultra_duty_pct) ||
	    !network_mode_wave_valid(candidate.wave) ||
	    !network_mode_accent_valid(candidate.accent_pct)) {
		return NETWORK_MODE_REJECTED;
	}
	k_mutex_lock(&g_mode_mutex, K_FOREVER);
	/* A record from a pre-v3 wire carries no opinion on the missing
	 * fields: fill them from the local register under the SAME lock
	 * the comparison runs under, so a concurrent adoption cannot slip
	 * in between. Inheritance fills values only — the known flags stay
	 * as the wire set them, which is what keeps an echo from winning
	 * provenance ties (see network_mode_compare). */
	if (!candidate.duty_known) {
		candidate.ultra_duty_pct = g_mode.ultra_duty_pct;
	}
	if (!candidate.wave_known) {
		candidate.wave = g_mode.wave;
	}
	if (!candidate.accent_known) {
		candidate.accent_pct = g_mode.accent_pct;
	}
	int comparison = network_mode_compare(&candidate, &g_mode);

	if (comparison <= 0) {
		enum network_mode_apply_result result = comparison == 0
			? NETWORK_MODE_UNCHANGED : NETWORK_MODE_REJECTED;

		k_mutex_unlock(&g_mode_mutex);
		return result;
	}
	if (!g_ready) {
		k_mutex_unlock(&g_mode_mutex);
		return NETWORK_MODE_STORAGE_ERROR;
	}
	struct mode_store updated = {
		.magic = MODE_MAGIC,
		.version = MODE_STORE_VERSION,
		.mode = (uint8_t)candidate.mode,
		.blink_cs = candidate.ultra_blink_cs,
		.duty_pct = candidate.ultra_duty_pct,
		.wave = candidate.wave,
		.accent_pct = candidate.accent_pct,
		.priority = candidate.priority,
	};

	updated.crc = store_crc(&updated);
	int rc = settings_save_one(MODE_STORE_KEY, &updated, sizeof(updated));

	if (rc != 0) {
		LOG_ERR("fleet mode save failed: %d", rc);
		k_mutex_unlock(&g_mode_mutex);
		return NETWORK_MODE_STORAGE_ERROR;
	}
	g_mode = candidate;
	g_generation++;
	LOG_INF("adopted fleet mode %s priority %u",
		network_mode_name(g_mode.mode), g_mode.priority);
	k_mutex_unlock(&g_mode_mutex);
	return NETWORK_MODE_ADOPTED;
}

enum network_mode_apply_result
network_mode_set(enum network_mode mode, uint32_t priority,
		 uint8_t ultra_blink_cs, uint8_t ultra_duty_pct,
		 uint8_t wave, uint8_t accent_pct)
{
	const struct network_mode_value candidate = {
		.mode = mode,
		.priority = priority,
		.ultra_blink_cs = ultra_blink_cs,
		.ultra_duty_pct = ultra_duty_pct,
		.wave = wave,
		.accent_pct = accent_pct,
		.duty_known = true,
		.wave_known = true,
		.accent_known = true,
	};

	return apply_value(&candidate);
}

bool network_mode_receive(const uint8_t *wire, size_t length)
{
	struct network_mode_value candidate;

	if (!network_mode_decode(&candidate, wire, length)) {
		return false;
	}
	/* Legacy-field inheritance happens inside apply_value, under the
	 * register lock. */
	(void)apply_value(&candidate);
	return true;
}

static uint32_t tx_jitter(uint32_t salt, uint32_t span)
{
	uint32_t value = g_node_id ^ salt ^ (g_generation * 0x9e3779b9U);

	value ^= value >> 16;
	value *= 0x7feb352dU;
	value ^= value >> 15;
	return span == 0 ? 0 : value % span;
}

void network_mode_tick(bool attached)
{
	int64_t now = k_uptime_get();
	struct network_mode_value value;
	bool send = false;

	k_mutex_lock(&g_mode_mutex, K_FOREVER);
	if (attached && !g_was_attached) {
		g_burst_remaining = MODE_GOSSIP_BURST_COUNT;
		g_next_tx_ms = now + 50 + tx_jitter(0x11U, 450);
	}
	if (attached && g_tx_generation != g_generation) {
		g_tx_generation = g_generation;
		g_burst_remaining = MODE_GOSSIP_BURST_COUNT;
		g_next_tx_ms = now + 50 + tx_jitter(0x22U, 450);
	}
	if (attached && now >= g_next_tx_ms) {
		value = g_mode;
		send = true;
		if (g_burst_remaining > 0) {
			g_burst_remaining--;
		}
		if (g_burst_remaining > 0) {
			g_next_tx_ms = now + 700 +
				tx_jitter((uint32_t)g_burst_remaining, 600);
		} else {
			g_next_tx_ms = now + MODE_GOSSIP_PERIOD_MS +
				tx_jitter(0x44U, 15000);
		}
	}
	g_was_attached = attached;
	k_mutex_unlock(&g_mode_mutex);

	if (send) {
		/* Rolling migration: send v4 (duty + wave + accent) for
		 * current nodes, v3 so not-yet-OTA'd nodes keep receiving
		 * duty/wave (they reject the longer v4 frame outright on
		 * length), plus the legacy v1 copy so the oldest targets
		 * keep following mode changes until their OTA. */
		uint8_t wire[NETWORK_MODE_WIRE_SIZE_V4];
		int length = network_mode_encode_v4(wire, sizeof(wire), &value);

		if (length > 0) {
			sync_send_aux(wire, (size_t)length);
		}
		length = network_mode_encode_v3(wire, sizeof(wire), &value);
		if (length > 0) {
			sync_send_aux(wire, (size_t)length);
		}
		length = network_mode_encode_v1(wire, sizeof(wire), &value);
		if (length > 0) {
			sync_send_aux(wire, (size_t)length);
		}
	}
}
