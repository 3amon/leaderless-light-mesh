#include "always_on.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(always_on, LOG_LEVEL_INF);

/* Same magic/version/CRC shape as the solo-mode and LED power records. */
#define ALWAYS_ON_MAGIC 0x414c4f4eU /* "ALON" */
#define ALWAYS_ON_VERSION 1
#define ALWAYS_ON_KEY "ringalwayson/mode"

struct always_on_store {
	uint32_t magic;
	uint8_t version;
	uint8_t always_on;
	uint8_t reserved[2];
	uint32_t crc;
} __packed;

BUILD_ASSERT(sizeof(struct always_on_store) <= SETTINGS_MAX_VAL_LEN);

static bool active;
static bool ready;

static uint32_t store_crc(const struct always_on_store *value)
{
	return crc32_ieee((const uint8_t *)value,
			  offsetof(struct always_on_store, crc));
}

static bool store_valid(const struct always_on_store *value)
{
	return value->magic == ALWAYS_ON_MAGIC &&
	       value->version == ALWAYS_ON_VERSION &&
	       value->always_on <= 1U &&
	       value->crc == store_crc(value);
}

static int load_cb(const char *key, size_t len, settings_read_cb read_cb,
		   void *cb_arg, void *param)
{
	struct always_on_store *loaded = param;

	if (strcmp(key, "mode") != 0 || len != sizeof(*loaded)) {
		return 0;
	}
	if (read_cb(cb_arg, loaded, sizeof(*loaded)) != sizeof(*loaded)) {
		memset(loaded, 0, sizeof(*loaded));
	}
	return 1;
}

int always_on_init(void)
{
	int rc = settings_subsys_init();

	active = false;
	ready = false;
	if (rc != 0) {
		LOG_ERR("settings init for always-on failed: %d", rc);
		return rc;
	}
	struct always_on_store loaded = { 0 };

	rc = settings_load_subtree_direct("ringalwayson", load_cb, &loaded);
	if (rc == 0 && store_valid(&loaded)) {
		active = loaded.always_on != 0U;
	}
	ready = rc == 0;
	LOG_INF("always-on %s", active ? "on" : "off");
	return rc;
}

bool always_on_get(void)
{
	return active;
}

int always_on_set(bool always_on)
{
	if (!ready) {
		return -EAGAIN;
	}
	if (always_on == active) {
		return 0;
	}
	struct always_on_store updated = {
		.magic = ALWAYS_ON_MAGIC,
		.version = ALWAYS_ON_VERSION,
		.always_on = always_on ? 1U : 0U,
	};

	updated.crc = store_crc(&updated);
	int rc = settings_save_one(ALWAYS_ON_KEY, &updated, sizeof(updated));

	if (rc == 0) {
		active = always_on;
		LOG_INF("saved always-on %s", always_on ? "on" : "off");
	} else {
		LOG_ERR("always-on save failed: %d", rc);
	}
	return rc;
}
