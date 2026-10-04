#include "led_config.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(led_config, LOG_LEVEL_INF);

#define LED_CONFIG_MAGIC 0x4c434647U /* "LCFG" */
#define LED_CONFIG_VERSION 1
#define LED_CONFIG_KEY "ringcfg/led"

struct led_config_store {
	uint32_t magic;
	uint8_t version;
	uint8_t layout;
	uint8_t reserved[2];
	uint32_t crc;
} __packed;

BUILD_ASSERT(sizeof(struct led_config_store) <= SETTINGS_MAX_VAL_LEN);

static enum led_layout active_layout = LED_LAYOUT_RING16;
static bool ready;

static uint32_t store_crc(const struct led_config_store *value)
{
	return crc32_ieee((const uint8_t *)value,
			  offsetof(struct led_config_store, crc));
}

static bool store_valid(const struct led_config_store *value)
{
	return value->magic == LED_CONFIG_MAGIC &&
	       value->version == LED_CONFIG_VERSION &&
	       led_layout_valid((enum led_layout)value->layout) &&
	       value->crc == store_crc(value);
}

static int load_cb(const char *key, size_t len, settings_read_cb read_cb,
		   void *cb_arg, void *param)
{
	struct led_config_store *loaded = param;

	if (strcmp(key, "led") != 0 || len != sizeof(*loaded)) {
		return 0;
	}
	if (read_cb(cb_arg, loaded, sizeof(*loaded)) != sizeof(*loaded)) {
		memset(loaded, 0, sizeof(*loaded));
	}
	return 1;
}

int led_config_init(void)
{
	int rc = settings_subsys_init();

	active_layout = LED_LAYOUT_RING16;
	ready = false;
	if (rc != 0) {
		LOG_ERR("settings init for LED layout failed: %d", rc);
		return rc;
	}
	struct led_config_store loaded = { 0 };

	rc = settings_load_subtree_direct("ringcfg", load_cb, &loaded);
	if (rc == 0 && store_valid(&loaded)) {
		active_layout = (enum led_layout)loaded.layout;
	}
	ready = rc == 0;
	LOG_INF("LED layout %s (%u pixels, default brightness %u%%)",
		led_layout_name(active_layout),
		led_layout_pixel_count(active_layout),
		led_layout_default_brightness(active_layout));
	return rc;
}

enum led_layout led_config_layout(void)
{
	return active_layout;
}

int led_config_set_layout(enum led_layout layout)
{
	if (!ready) {
		return -EAGAIN;
	}
	if (!led_layout_valid(layout)) {
		return -EINVAL;
	}
	if (layout == active_layout) {
		return 0;
	}
	struct led_config_store updated = {
		.magic = LED_CONFIG_MAGIC,
		.version = LED_CONFIG_VERSION,
		.layout = (uint8_t)layout,
	};

	updated.crc = store_crc(&updated);
	int rc = settings_save_one(LED_CONFIG_KEY, &updated, sizeof(updated));

	if (rc == 0) {
		active_layout = layout;
		LOG_INF("saved LED layout %s", led_layout_name(layout));
	} else {
		LOG_ERR("persistent LED layout save failed: %d", rc);
	}
	return rc;
}
