#include "solo_mode.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(solo_mode, LOG_LEVEL_INF);

#define SOLO_MAGIC 0x534f4c4fU /* "SOLO" */
#define SOLO_VERSION 1
#define SOLO_KEY "ringsolo/mode"

struct solo_store {
	uint32_t magic;
	uint8_t version;
	uint8_t solo;
	uint8_t reserved[2];
	uint32_t crc;
} __packed;

BUILD_ASSERT(sizeof(struct solo_store) <= SETTINGS_MAX_VAL_LEN);

static bool active_solo;
static bool ready;

static uint32_t store_crc(const struct solo_store *value)
{
	return crc32_ieee((const uint8_t *)value,
			  offsetof(struct solo_store, crc));
}

static bool store_valid(const struct solo_store *value)
{
	return value->magic == SOLO_MAGIC &&
	       value->version == SOLO_VERSION &&
	       value->solo <= 1U &&
	       value->crc == store_crc(value);
}

static int load_cb(const char *key, size_t len, settings_read_cb read_cb,
		   void *cb_arg, void *param)
{
	struct solo_store *loaded = param;

	if (strcmp(key, "mode") != 0 || len != sizeof(*loaded)) {
		return 0;
	}
	if (read_cb(cb_arg, loaded, sizeof(*loaded)) != sizeof(*loaded)) {
		memset(loaded, 0, sizeof(*loaded));
	}
	return 1;
}

int solo_mode_init(void)
{
	int rc = settings_subsys_init();

	/* Absent or corrupt record means fleet behaviour. Self-healing is the
	 * safer default: a node wrongly told it is solo would sit in a wedged
	 * partition forever, while one wrongly healing merely restarts. */
	active_solo = false;
	ready = false;
	if (rc != 0) {
		LOG_ERR("settings init for solo mode failed: %d", rc);
		return rc;
	}
	struct solo_store loaded = { 0 };

	rc = settings_load_subtree_direct("ringsolo", load_cb, &loaded);
	if (rc == 0 && store_valid(&loaded)) {
		active_solo = loaded.solo != 0U;
	}
	ready = rc == 0;
	LOG_INF("solo mode %s", active_solo ? "on" : "off");
	return rc;
}

bool solo_mode_get(void)
{
	return active_solo;
}

int solo_mode_set(bool solo)
{
	if (!ready) {
		return -EAGAIN;
	}
	if (solo == active_solo) {
		return 0;
	}
	struct solo_store updated = {
		.magic = SOLO_MAGIC,
		.version = SOLO_VERSION,
		.solo = solo ? 1U : 0U,
	};

	updated.crc = store_crc(&updated);
	int rc = settings_save_one(SOLO_KEY, &updated, sizeof(updated));

	if (rc == 0) {
		active_solo = solo;
		LOG_INF("saved solo mode %s", solo ? "on" : "off");
	} else {
		LOG_ERR("solo mode save failed: %d", rc);
	}
	return rc;
}
