#include "boot_status.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(boot_status, LOG_LEVEL_INF);

#define BOOT_STATUS_MAGIC 0x42535452U /* "BSTR" */
#define BOOT_STATUS_VERSION 2
#define BOOT_STATUS_EVENTS 8
#define BOOT_STATUS_KEY "ringboot/state"

struct boot_event {
	uint32_t boot;
	uint32_t uptime_ms;
	uint32_t reset_cause;
	uint8_t reboot_reason;
	uint8_t stage;
	uint8_t reserved[2];
} __packed;

struct boot_store {
	uint32_t magic;
	uint8_t version;
	uint8_t head;
	uint8_t count;
	uint8_t heal_attempts;   /* was: reserved. Old records read 0, which is
				  * the correct default, so no version bump. */
	uint32_t boot;
	struct boot_event events[BOOT_STATUS_EVENTS];
	uint32_t crc;
} __packed;

BUILD_ASSERT(sizeof(struct boot_store) <= SETTINGS_MAX_VAL_LEN);

static struct boot_store store;
static enum boot_stage current_stage;
static uint8_t current_mcuboot_phase;
static uint8_t current_mcuboot_detail;
static bool ready;

static uint32_t store_crc(const struct boot_store *value)
{
	return crc32_ieee((const uint8_t *)value,
			  offsetof(struct boot_store, crc));
}

static int load_cb(const char *key, size_t len, settings_read_cb read_cb,
		   void *cb_arg, void *param)
{
	struct boot_store *loaded = param;

	ARG_UNUSED(key);
	if (len != sizeof(*loaded)) {
		return 1;
	}
	if (read_cb(cb_arg, loaded, sizeof(*loaded)) != sizeof(*loaded)) {
		memset(loaded, 0, sizeof(*loaded));
	}
	return 1;
}

static bool store_valid(const struct boot_store *value)
{
	return value->magic == BOOT_STATUS_MAGIC &&
	       value->version == BOOT_STATUS_VERSION &&
	       value->head < BOOT_STATUS_EVENTS &&
	       value->count <= BOOT_STATUS_EVENTS &&
	       value->crc == store_crc(value);
}

static int save(void)
{
	store.crc = store_crc(&store);
	int rc = settings_save_one(BOOT_STATUS_KEY, &store, sizeof(store));

	if (rc != 0) {
		LOG_ERR("persistent boot breadcrumb save failed: %d", rc);
	}
	return rc;
}

int boot_status_init(uint32_t reset_cause, uint8_t reboot_reason,
		     uint8_t mcuboot_phase, uint8_t mcuboot_detail)
{
	int rc = settings_subsys_init();

	if (rc != 0) {
		LOG_ERR("settings init for boot breadcrumbs failed: %d", rc);
		return rc;
	}

	struct boot_store loaded = { 0 };

	rc = settings_load_subtree_direct(BOOT_STATUS_KEY, load_cb, &loaded);
	if (rc != 0 || !store_valid(&loaded)) {
		memset(&store, 0, sizeof(store));
		store.magic = BOOT_STATUS_MAGIC;
		store.version = BOOT_STATUS_VERSION;
	} else {
		store = loaded;
	}

	store.boot++;
	if (store.boot == 0) {
		store.boot = 1;
	}
	current_stage = 0;
	current_mcuboot_phase = mcuboot_phase;
	current_mcuboot_detail = mcuboot_detail;
	ready = true;

	struct boot_event *event = &store.events[store.head];

	memset(event, 0, sizeof(*event));
	event->boot = store.boot;
	event->reset_cause = reset_cause;
	event->reboot_reason = reboot_reason;
	event->stage = BOOT_STAGE_ENTRY;
	event->reserved[0] = mcuboot_phase;
	event->reserved[1] = mcuboot_detail;
	store.head = (store.head + 1) % BOOT_STATUS_EVENTS;
	store.count = MIN(store.count + 1, BOOT_STATUS_EVENTS);
	current_stage = BOOT_STAGE_ENTRY;

	rc = save();
	LOG_INF("boot breadcrumb %u: %s (reset %08x, reason %02x)",
		store.boot, boot_status_stage_name(current_stage), reset_cause,
		reboot_reason);
	return rc;
}

int boot_status_record(enum boot_stage stage)
{
	if (!ready) {
		return -EAGAIN;
	}
	if (stage <= current_stage || stage > BOOT_STAGE_CONFIRMED) {
		return 0;
	}

	struct boot_event *event = &store.events[store.head];

	memset(event, 0, sizeof(*event));
	event->boot = store.boot;
	event->uptime_ms = (uint32_t)k_uptime_get();
	event->stage = stage;
	store.head = (store.head + 1) % BOOT_STATUS_EVENTS;
	store.count = MIN(store.count + 1, BOOT_STATUS_EVENTS);
	current_stage = stage;
	LOG_INF("boot breadcrumb %u: %s at %u ms", store.boot,
		boot_status_stage_name(stage), event->uptime_ms);
	return save();
}

uint8_t boot_status_heal_attempts(void)
{
	return ready ? store.heal_attempts : 0;
}

/* Persisted rather than kept in RAM: the rejoin backoff has to survive the very
 * reboot it is counting. __noinit RAM does not -- MCUboot is chainloaded ahead
 * of the application and uses RAM itself -- so the counter read back as zero
 * every boot and the interval never grew past the first step. */
int boot_status_set_heal_attempts(uint8_t attempts)
{
	if (!ready || store.heal_attempts == attempts) {
		return 0;   /* never spend a flash write on an unchanged value */
	}
	store.heal_attempts = attempts;
	return save();
}

uint32_t boot_status_sequence(void)
{
	return ready ? store.boot : 0;
}

enum boot_stage boot_status_stage(void)
{
	return current_stage;
}

const char *boot_status_stage_name(enum boot_stage stage)
{
	switch (stage) {
	case BOOT_STAGE_ENTRY:          return "entry";
	case BOOT_STAGE_WATCHDOG:       return "watchdog";
	case BOOT_STAGE_USB_QUIESCED:   return "usb-quiesced";
	case BOOT_STAGE_USB:            return "usb";
	case BOOT_STAGE_ATTACHED:       return "attached";
	case BOOT_STAGE_INFRASTRUCTURE: return "infra";
	case BOOT_STAGE_CONFIRMED:      return "confirmed";
	default:                        return "none";
	}
}

static uint32_t escape_attempts;

void boot_status_note_escape_attempts(uint32_t attempts)
{
	escape_attempts = attempts;
}

int boot_status_format(char *buf, size_t len)
{
	int used = snprintf(buf, len,
			    "boot=%u stage=%s mcuboot=%02x/%02x ladder=%u trail=",
			    boot_status_sequence(),
			    boot_status_stage_name(current_stage),
			    current_mcuboot_phase, current_mcuboot_detail,
			    escape_attempts);

	if (used < 0 || (size_t)used >= len) {
		return -ENOSPC;
	}

	for (uint8_t i = 0; i < store.count; i++) {
		uint8_t idx = (store.head + BOOT_STATUS_EVENTS - store.count + i) %
			      BOOT_STATUS_EVENTS;
		const struct boot_event *event = &store.events[idx];
		int n = snprintf(buf + used, len - used, "%s%u:%s@%u",
				 i ? "," : "", event->boot,
				 boot_status_stage_name(event->stage),
				 event->uptime_ms);

		if (n < 0 || (size_t)n >= len - used) {
			return -ENOSPC;
		}
		used += n;
	}
	return used;
}
