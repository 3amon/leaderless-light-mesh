/* Persistent authentication and replay protection for UDP mutations. */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "control_protocol.h"

#define CONTROL_AUTH_KEY_BYTES 32

enum control_auth_result {
	CONTROL_AUTH_ACCEPT_NEW,
	CONTROL_AUTH_ACCEPT_DUPLICATE,
	CONTROL_AUTH_UNPROVISIONED,
	CONTROL_AUTH_BAD_TAG,
	CONTROL_AUTH_REPLAY,
	CONTROL_AUTH_STORAGE_ERROR,
	CONTROL_AUTH_CRYPTO_ERROR,
};

int control_auth_init(void);
bool control_auth_ready(void);
uint64_t control_auth_sequence(void);
enum control_auth_result control_auth_authorize(
	const void *message, size_t message_length, uint64_t sequence,
	const uint8_t tag[CONTROL_AUTH_TAG_BYTES]);
int control_auth_sign(const void *message, size_t message_length,
		      uint8_t tag[CONTROL_AUTH_TAG_BYTES]);
const char *control_auth_result_name(enum control_auth_result result);
int control_auth_provision_hex(const char *hex_key);
