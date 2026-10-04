#include "led_power_mode.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(led_power_mode, LOG_LEVEL_INF);

#define LED_POWER_MAGIC 0x4c505752U /* "LPWR" */
#define LED_POWER_VERSION 1
#define LED_POWER_KEY "ringpower/mode"

struct led_power_store {
	uint32_t magic;
	uint8_t version;
	uint8_t mode;
	uint8_t reserved[2];
	uint32_t crc;
} __packed;

BUILD_ASSERT(sizeof(struct led_power_store) <= SETTINGS_MAX_VAL_LEN);

static enum led_power_mode active_mode = LED_POWER_MODULE_VCC;
static bool ready;

static uint32_t store_crc(const struct led_power_store *value)
{
	return crc32_ieee((const uint8_t *)value,
			  offsetof(struct led_power_store, crc));
}

bool led_power_mode_valid(enum led_power_mode mode)
{
	return mode == LED_POWER_CARRIER || mode == LED_POWER_MODULE_VCC;
}

const char *led_power_mode_name(enum led_power_mode mode)
{
	return mode == LED_POWER_MODULE_VCC ? "module-vcc" : "carrier";
}

static bool store_valid(const struct led_power_store *value)
{
	return value->magic == LED_POWER_MAGIC &&
	       value->version == LED_POWER_VERSION &&
	       led_power_mode_valid((enum led_power_mode)value->mode) &&
	       value->crc == store_crc(value);
}

static int load_cb(const char *key, size_t len, settings_read_cb read_cb,
		   void *cb_arg, void *param)
{
	struct led_power_store *loaded = param;

	if (strcmp(key, "mode") != 0 || len != sizeof(*loaded)) {
		return 0;
	}
	if (read_cb(cb_arg, loaded, sizeof(*loaded)) != sizeof(*loaded)) {
		memset(loaded, 0, sizeof(*loaded));
	}
	return 1;
}

int led_power_mode_init(void)
{
	int rc = settings_subsys_init();

	/* New/unconfigured low-pixel-count assemblies power the strip from the
	 * module's switchable 3.3 V VCC output. A valid saved per-device choice
	 * below still takes precedence, including the legacy carrier backend. */
	active_mode = LED_POWER_MODULE_VCC;
	ready = false;
	if (rc != 0) {
		LOG_ERR("settings init for LED power mode failed: %d", rc);
		return rc;
	}
	struct led_power_store loaded = { 0 };

	rc = settings_load_subtree_direct("ringpower", load_cb, &loaded);
	if (rc == 0 && store_valid(&loaded)) {
		active_mode = (enum led_power_mode)loaded.mode;
	}
	ready = rc == 0;
	LOG_INF("LED power backend %s", led_power_mode_name(active_mode));
	return rc;
}

enum led_power_mode led_power_mode_get(void)
{
	return active_mode;
}

int led_power_mode_set(enum led_power_mode mode)
{
	if (!ready) {
		return -EAGAIN;
	}
	if (!led_power_mode_valid(mode)) {
		return -EINVAL;
	}
	if (mode == active_mode) {
		return 0;
	}
	struct led_power_store updated = {
		.magic = LED_POWER_MAGIC,
		.version = LED_POWER_VERSION,
		.mode = (uint8_t)mode,
	};

	updated.crc = store_crc(&updated);
	int rc = settings_save_one(LED_POWER_KEY, &updated, sizeof(updated));

	if (rc == 0) {
		active_mode = mode;
		LOG_INF("saved LED power backend %s", led_power_mode_name(mode));
	} else {
		LOG_ERR("persistent LED power mode save failed: %d", rc);
	}
	return rc;
}
