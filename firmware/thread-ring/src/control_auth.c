#include "control_auth.h"

#include <errno.h>
#include <mbedtls/md.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>

LOG_MODULE_REGISTER(control_auth, LOG_LEVEL_INF);

#define CONTROL_AUTH_MAGIC 0x52415554U /* "RAUT" */
#define CONTROL_AUTH_VERSION 1
#define CONTROL_AUTH_SETTINGS_KEY "ringauth/state"

struct control_auth_store {
	uint32_t magic;
	uint8_t version;
	uint8_t reserved[3];
	uint8_t key[CONTROL_AUTH_KEY_BYTES];
	uint64_t sequence;
	uint8_t tag[CONTROL_AUTH_TAG_BYTES];
	uint32_t crc;
} __packed;

static struct control_auth_store store;
static bool ready;
K_MUTEX_DEFINE(auth_lock);

static uint32_t store_crc(const struct control_auth_store *value)
{
	return crc32_ieee((const uint8_t *)value,
			  offsetof(struct control_auth_store, crc));
}

static bool store_valid(const struct control_auth_store *value)
{
	return value->magic == CONTROL_AUTH_MAGIC &&
	       value->version == CONTROL_AUTH_VERSION &&
	       value->crc == store_crc(value);
}

static int load_cb(const char *key, size_t len, settings_read_cb read_cb,
		   void *cb_arg, void *param)
{
	struct control_auth_store *loaded = param;

	if (strcmp(key, "state") != 0 || len != sizeof(*loaded)) {
		return 0;
	}
	if (read_cb(cb_arg, loaded, sizeof(*loaded)) != sizeof(*loaded)) {
		memset(loaded, 0, sizeof(*loaded));
	}
	return 1;
}

static int save(struct control_auth_store *value)
{
	value->crc = store_crc(value);
	return settings_save_one(CONTROL_AUTH_SETTINGS_KEY, value, sizeof(*value));
}

static bool constant_time_equal(const uint8_t *left, const uint8_t *right,
				size_t length)
{
	uint8_t difference = 0;

	for (size_t index = 0; index < length; index++) {
		difference |= left[index] ^ right[index];
	}
	return difference == 0;
}

static int calculate_tag(const uint8_t key[CONTROL_AUTH_KEY_BYTES],
			 const void *message, size_t message_length,
			 uint8_t tag[CONTROL_AUTH_TAG_BYTES])
{
	const mbedtls_md_info_t *info =
		mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	uint8_t digest[32];

	if (info == NULL) {
		return -ENOTSUP;
	}
	int rc = mbedtls_md_hmac(info, key, CONTROL_AUTH_KEY_BYTES, message,
				 message_length, digest);

	if (rc == 0) {
		memcpy(tag, digest, CONTROL_AUTH_TAG_BYTES);
	}
	memset(digest, 0, sizeof(digest));
	return rc;
}

int control_auth_init(void)
{
	int rc = settings_subsys_init();

	if (rc != 0) {
		LOG_ERR("settings init for control authentication failed: %d", rc);
		return rc;
	}
	struct control_auth_store loaded = { 0 };

	rc = settings_load_subtree_direct("ringauth", load_cb, &loaded);
	k_mutex_lock(&auth_lock, K_FOREVER);
	if (rc == 0 && store_valid(&loaded)) {
		store = loaded;
		ready = true;
		LOG_INF("control authentication ready at sequence %llu",
			(unsigned long long)store.sequence);
	} else {
		memset(&store, 0, sizeof(store));
		ready = false;
		LOG_WRN("control authentication is unprovisioned; mutations disabled");
	}
	k_mutex_unlock(&auth_lock);
	memset(&loaded, 0, sizeof(loaded));
	return rc;
}

bool control_auth_ready(void)
{
	k_mutex_lock(&auth_lock, K_FOREVER);
	bool result = ready;
	k_mutex_unlock(&auth_lock);
	return result;
}

uint64_t control_auth_sequence(void)
{
	k_mutex_lock(&auth_lock, K_FOREVER);
	uint64_t result = ready ? store.sequence : 0;
	k_mutex_unlock(&auth_lock);
	return result;
}

enum control_auth_result control_auth_authorize(
	const void *message, size_t message_length, uint64_t sequence,
	const uint8_t tag[CONTROL_AUTH_TAG_BYTES])
{
	uint8_t calculated[CONTROL_AUTH_TAG_BYTES];
	enum control_auth_result result;

	k_mutex_lock(&auth_lock, K_FOREVER);
	if (!ready) {
		result = CONTROL_AUTH_UNPROVISIONED;
		goto out;
	}
	if (calculate_tag(store.key, message, message_length, calculated) != 0) {
		result = CONTROL_AUTH_CRYPTO_ERROR;
		goto out;
	}
	if (!constant_time_equal(calculated, tag, sizeof(calculated))) {
		result = CONTROL_AUTH_BAD_TAG;
		goto clear;
	}
	if (sequence < store.sequence ||
	    (sequence == store.sequence &&
	     !constant_time_equal(tag, store.tag, sizeof(store.tag)))) {
		result = CONTROL_AUTH_REPLAY;
		goto clear;
	}
	if (sequence == store.sequence) {
		result = CONTROL_AUTH_ACCEPT_DUPLICATE;
		goto clear;
	}
	struct control_auth_store updated = store;

	updated.sequence = sequence;
	memcpy(updated.tag, tag, sizeof(updated.tag));
	if (save(&updated) != 0) {
		result = CONTROL_AUTH_STORAGE_ERROR;
		goto clear;
	}
	store = updated;
	result = CONTROL_AUTH_ACCEPT_NEW;

clear:
	memset(calculated, 0, sizeof(calculated));
out:
	k_mutex_unlock(&auth_lock);
	return result;
}

int control_auth_sign(const void *message, size_t message_length,
		      uint8_t tag[CONTROL_AUTH_TAG_BYTES])
{
	k_mutex_lock(&auth_lock, K_FOREVER);
	int rc = ready ? calculate_tag(store.key, message, message_length, tag)
		       : -EACCES;
	k_mutex_unlock(&auth_lock);
	return rc;
}

const char *control_auth_result_name(enum control_auth_result result)
{
	switch (result) {
	case CONTROL_AUTH_ACCEPT_NEW: return "ok";
	case CONTROL_AUTH_ACCEPT_DUPLICATE: return "duplicate";
	case CONTROL_AUTH_UNPROVISIONED: return "auth_unprovisioned";
	case CONTROL_AUTH_BAD_TAG: return "auth_failed";
	case CONTROL_AUTH_REPLAY: return "replay";
	case CONTROL_AUTH_STORAGE_ERROR: return "auth_storage";
	case CONTROL_AUTH_CRYPTO_ERROR: return "auth_crypto";
	default: return "auth_error";
	}
}

static int hex_nibble(char value)
{
	if (value >= '0' && value <= '9') {
		return value - '0';
	}
	if (value >= 'a' && value <= 'f') {
		return value - 'a' + 10;
	}
	if (value >= 'A' && value <= 'F') {
		return value - 'A' + 10;
	}
	return -1;
}

int control_auth_provision_hex(const char *hex_key)
{
	if (strlen(hex_key) != CONTROL_AUTH_KEY_BYTES * 2) {
		return -EINVAL;
	}
	struct control_auth_store provisioned = {
		.magic = CONTROL_AUTH_MAGIC,
		.version = CONTROL_AUTH_VERSION,
	};

	for (size_t index = 0; index < CONTROL_AUTH_KEY_BYTES; index++) {
		int high = hex_nibble(hex_key[index * 2]);
		int low = hex_nibble(hex_key[index * 2 + 1]);

		if (high < 0 || low < 0) {
			memset(&provisioned, 0, sizeof(provisioned));
			return -EINVAL;
		}
		provisioned.key[index] = (uint8_t)((high << 4) | low);
	}

	k_mutex_lock(&auth_lock, K_FOREVER);
	int rc = save(&provisioned);

	if (rc == 0) {
		store = provisioned;
		ready = true;
		LOG_INF("control authentication key provisioned; sequence reset");
	}
	k_mutex_unlock(&auth_lock);
	memset(&provisioned, 0, sizeof(provisioned));
	return rc;
}
