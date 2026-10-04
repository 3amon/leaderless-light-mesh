#include "control_protocol.h"
#include "ota_quiet.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define CONTROL_MAX_TOKENS ((CONTROL_MAX_DATAGRAM + 1) / 2)

struct tokens {
	char text[CONTROL_MAX_DATAGRAM + 1];
	char *item[CONTROL_MAX_TOKENS];
	size_t count;
};

static bool request_id_valid(const char *text)
{
	size_t length = strlen(text);

	if (length == 0 || length > CONTROL_REQUEST_ID_MAX) {
		return false;
	}
	for (size_t index = 0; index < length; index++) {
		char c = text[index];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '-' || c == '_')) {
			return false;
		}
	}
	return true;
}

static enum control_parse_result tokenize(const void *data, size_t length,
					   struct tokens *tokens)
{
	memset(tokens, 0, sizeof(*tokens));
	if (length == 0) {
		return CONTROL_PARSE_EMPTY;
	}
	if (length > CONTROL_MAX_DATAGRAM) {
		return CONTROL_PARSE_TOO_LONG;
	}
	memcpy(tokens->text, data, length);
	tokens->text[length] = '\0';

	for (size_t index = 0; index < length; index++) {
		unsigned char c = (unsigned char)tokens->text[index];

		if (c == 0 || c < 0x20 || c > 0x7e) {
			return CONTROL_PARSE_INVALID_TEXT;
		}
	}
	if (tokens->text[0] == ' ' || tokens->text[length - 1] == ' ') {
		return CONTROL_PARSE_BAD_ARGUMENTS;
	}

	tokens->item[tokens->count++] = tokens->text;
	for (size_t index = 0; index < length; index++) {
		if (tokens->text[index] != ' ') {
			continue;
		}
		if (index + 1 >= length || tokens->text[index + 1] == ' ') {
			return CONTROL_PARSE_BAD_ARGUMENTS;
		}
		tokens->text[index] = '\0';
		if (tokens->count >= CONTROL_MAX_TOKENS) {
			return CONTROL_PARSE_BAD_ARGUMENTS;
		}
		tokens->item[tokens->count++] = &tokens->text[index + 1];
	}
	return CONTROL_PARSE_OK;
}

static bool parse_integer(const char *text, int64_t minimum, int64_t maximum,
			  int64_t *value)
{
	bool negative = false;
	size_t index = 0;
	uint64_t magnitude = 0;
	uint64_t limit;

	if (text[0] == '-') {
		negative = true;
		index = 1;
	}
	if (text[index] == '\0') {
		return false;
	}
	limit = negative ? (uint64_t)(-(minimum + 1)) + 1U : (uint64_t)maximum;
	for (; text[index] != '\0'; index++) {
		if (text[index] < '0' || text[index] > '9') {
			return false;
		}
		unsigned int digit = (unsigned int)(text[index] - '0');

		if ((uint64_t)digit > limit ||
		    magnitude > (limit - (uint64_t)digit) / 10U) {
			return false;
		}
		magnitude = magnitude * 10U + digit;
	}
	if (negative) {
		*value = magnitude == (uint64_t)INT64_MAX + 1U
				 ? INT64_MIN
				 : -(int64_t)magnitude;
	} else {
		*value = (int64_t)magnitude;
	}
	return *value >= minimum && *value <= maximum;
}

static bool no_arguments(size_t command_end, size_t command_index)
{
	return command_end == command_index + 1;
}

static int hex_nibble(char value)
{
	if (value >= '0' && value <= '9') {
		return value - '0';
	}
	if (value >= 'a' && value <= 'f') {
		return value - 'a' + 10;
	}
	return -1;
}

static bool parse_sequence(const char *text, uint64_t *sequence)
{
	if (strlen(text) != 16) {
		return false;
	}
	uint64_t parsed = 0;

	for (size_t index = 0; index < 16; index++) {
		int nibble = hex_nibble(text[index]);

		if (nibble < 0) {
			return false;
		}
		parsed = (parsed << 4) | (uint64_t)nibble;
	}
	*sequence = parsed;
	return true;
}

static bool parse_tag(const char *text, uint8_t *tag)
{
	if (strlen(text) != CONTROL_AUTH_TAG_HEX_LENGTH) {
		return false;
	}
	for (size_t index = 0; index < CONTROL_AUTH_TAG_BYTES; index++) {
		int high = hex_nibble(text[index * 2]);
		int low = hex_nibble(text[index * 2 + 1]);

		if (high < 0 || low < 0) {
			return false;
		}
		tag[index] = (uint8_t)((high << 4) | low);
	}
	return true;
}

static bool command_name_known(const char *name)
{
	static const char *const names[] = {
		"status", "group", "boot", "auth", "flash", "scramble", "update",
		"reboot", "sleep", "skew", "pattern", "bright", "layout", "power", "hb",
		"gossip", "quiet", "mode", "heal",
	};

	for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		if (strcmp(name, names[index]) == 0) {
			return true;
		}
	}
	return false;
}

enum control_parse_result control_parse(const void *data, size_t length,
					int pattern_count,
					struct control_command *command)
{
	struct tokens tokens;
	enum control_parse_result result = tokenize(data, length, &tokens);

	memset(command, 0, sizeof(*command));
	if (result != CONTROL_PARSE_OK) {
		return result;
	}

	size_t command_index = 0;
	size_t command_end = tokens.count;

	if (strcmp(tokens.item[0], "v1") == 0) {
		if (tokens.count < 3 || !request_id_valid(tokens.item[1])) {
			return CONTROL_PARSE_BAD_ENVELOPE;
		}
		command->versioned = true;
		strcpy(command->request_id, tokens.item[1]);
		command_index = 2;
	} else if (strcmp(tokens.item[0], "v2") == 0) {
		if (tokens.count < 4 ||
		    !parse_sequence(tokens.item[1], &command->sequence) ||
		    !parse_tag(tokens.item[tokens.count - 1], command->auth_tag)) {
			return CONTROL_PARSE_BAD_ENVELOPE;
		}
		command->versioned = true;
		command->authenticated = true;
		command->signed_length =
			length - strlen(tokens.item[tokens.count - 1]) - 1;
		command_index = 2;
		command_end--;
	}

	const char *name = tokens.item[command_index];
	size_t argument_count = command_end - command_index - 1;
	char **arguments = &tokens.item[command_index + 1];

	if (strcmp(name, "status") == 0 && no_arguments(command_end, command_index)) {
		command->type = CONTROL_STATUS;
		command->query = true;
	} else if (strcmp(name, "group") == 0 &&
		   no_arguments(command_end, command_index)) {
		command->type = CONTROL_GROUP;
		command->query = true;
	} else if (strcmp(name, "boot") == 0 &&
		   no_arguments(command_end, command_index)) {
		command->type = CONTROL_BOOT;
		command->query = true;
	} else if (strcmp(name, "auth") == 0 &&
		   no_arguments(command_end, command_index)) {
		command->type = CONTROL_AUTH_STATUS;
		command->query = true;
	} else if (strcmp(name, "update") == 0 &&
		   no_arguments(command_end, command_index)) {
		command->type = CONTROL_UPDATE;
	} else if (strcmp(name, "reboot") == 0 &&
		   no_arguments(command_end, command_index)) {
		command->type = CONTROL_REBOOT;
	} else if (strcmp(name, "sleep") == 0 &&
		   no_arguments(command_end, command_index)) {
		command->type = CONTROL_SLEEP;
	} else if (strcmp(name, "hb") == 0 &&
		   no_arguments(command_end, command_index)) {
		command->type = CONTROL_HEARTBEAT;
	} else if (strcmp(name, "flash") == 0 && argument_count == 1) {
		command->type = CONTROL_FLASH;
		if (strcmp(arguments[0], "off") == 0) {
			command->value = 0;
		} else if (!parse_integer(arguments[0], 200, 60000,
					  &command->value)) {
			return CONTROL_PARSE_OUT_OF_RANGE;
		}
	} else if (strcmp(name, "scramble") == 0 && argument_count == 1) {
		command->type = CONTROL_SCRAMBLE;
		if (!parse_integer(arguments[0], -600000, 600000,
				   &command->value)) {
			return CONTROL_PARSE_OUT_OF_RANGE;
		}
	} else if (strcmp(name, "skew") == 0 && argument_count == 1) {
		command->type = CONTROL_SKEW;
		if (strcmp(arguments[0], "off") == 0) {
			command->value = 0;
		} else if (!parse_integer(arguments[0], -6000, 6000,
					  &command->value)) {
			return CONTROL_PARSE_OUT_OF_RANGE;
		}
	} else if (strcmp(name, "pattern") == 0 && argument_count == 1) {
		command->type = CONTROL_PATTERN;
		if (strcmp(arguments[0], "auto") == 0) {
			command->value = -1;
		} else if (pattern_count <= 0 ||
			   !parse_integer(arguments[0], 0, pattern_count - 1,
					  &command->value)) {
			return CONTROL_PARSE_OUT_OF_RANGE;
		}
	} else if (strcmp(name, "always-on") == 0 && argument_count == 0) {
		command->type = CONTROL_ALWAYS_ON;
		command->query = true;
	} else if (strcmp(name, "always-on") == 0 && argument_count == 1) {
		command->type = CONTROL_ALWAYS_ON;
		/* Permanently USB-powered install: never show the quiet
		 * charging display, always render. Persists. */
		if (strcmp(arguments[0], "on") == 0) {
			command->value = 1;
		} else if (strcmp(arguments[0], "off") == 0) {
			command->value = 0;
		} else {
			return CONTROL_PARSE_BAD_ARGUMENTS;
		}
	} else if (strcmp(name, "solo") == 0 && argument_count == 0) {
		command->type = CONTROL_SOLO_MODE;
		command->query = true;
	} else if (strcmp(name, "solo") == 0 && argument_count == 1) {
		command->type = CONTROL_SOLO_MODE;
		/* Deliberate single-node install: suppresses the solitude
		 * restart only, and persists across the reboot it prevents. */
		if (strcmp(arguments[0], "on") == 0) {
			command->value = 1;
		} else if (strcmp(arguments[0], "off") == 0) {
			command->value = 0;
		} else {
			return CONTROL_PARSE_BAD_ARGUMENTS;
		}
	} else if (strcmp(name, "budget") == 0 && argument_count == 0) {
		command->type = CONTROL_LED_BUDGET;
		command->query = true;
	} else if (strcmp(name, "budget") == 0 && argument_count == 1) {
		command->type = CONTROL_LED_BUDGET;
		/* per-LED summed channel units; "off" disables the cap. A
		 * floor of 30 keeps a mistyped value from blanking the ring —
		 * the ordinary scene already sits near that. */
		if (strcmp(arguments[0], "off") == 0) {
			command->value = 0;
		} else if (!parse_integer(arguments[0], 30, 765, &command->value)) {
			return CONTROL_PARSE_OUT_OF_RANGE;
		}
	} else if (strcmp(name, "bright") == 0 && argument_count == 1) {
		command->type = CONTROL_BRIGHTNESS;
		if (!parse_integer(arguments[0], 1, 100, &command->value)) {
			return CONTROL_PARSE_OUT_OF_RANGE;
		}
	} else if (strcmp(name, "layout") == 0 && argument_count == 0) {
		command->type = CONTROL_LED_LAYOUT;
		command->query = true;
	} else if (strcmp(name, "layout") == 0 && argument_count == 1) {
		command->type = CONTROL_LED_LAYOUT;
		if (strcmp(arguments[0], "ring16") == 0) {
			command->value = 16;
		} else if (strcmp(arguments[0], "jewel7") == 0) {
			command->value = 7;
		} else if (strcmp(arguments[0], "rope100") == 0) {
			command->value = 100;
		} else {
			return CONTROL_PARSE_BAD_ARGUMENTS;
		}
	} else if (strcmp(name, "power") == 0 && argument_count == 0) {
		command->type = CONTROL_LED_POWER;
		command->query = true;
	} else if (strcmp(name, "power") == 0 && argument_count == 1) {
		command->type = CONTROL_LED_POWER;
		if (strcmp(arguments[0], "carrier") == 0) {
			command->value = 0;
		} else if (strcmp(arguments[0], "module-vcc") == 0) {
			command->value = 1;
		} else {
			return CONTROL_PARSE_BAD_ARGUMENTS;
		}
	} else if (strcmp(name, "gossip") == 0 && argument_count == 1) {
		command->type = CONTROL_GOSSIP;
		if (strcmp(arguments[0], "local") == 0) {
			command->value = CONTROL_GOSSIP_LOCAL;
		} else if (strcmp(arguments[0], "realm") == 0) {
			command->value = CONTROL_GOSSIP_REALM;
		} else if (strcmp(arguments[0], "both") == 0) {
			command->value = CONTROL_GOSSIP_BOTH;
		} else {
			return CONTROL_PARSE_BAD_ARGUMENTS;
		}
	} else if (strcmp(name, "quiet") == 0 && argument_count == 0) {
		command->type = CONTROL_OTA_QUIET;
		command->query = true;
	} else if (strcmp(name, "quiet") == 0 && argument_count == 1) {
		command->type = CONTROL_OTA_QUIET;
		if (strcmp(arguments[0], "off") == 0) {
			command->value = 0;
		} else if (!parse_integer(arguments[0], OTA_QUIET_MIN_SECONDS,
					  OTA_QUIET_MAX_SECONDS, &command->value)) {
			return CONTROL_PARSE_OUT_OF_RANGE;
		}
	} else if (strcmp(name, "mode") == 0 && argument_count == 0) {
		command->type = CONTROL_NETWORK_MODE;
		command->query = true;
	} else if (strcmp(name, "mode") == 0 &&
		   (argument_count >= 2 && argument_count <= 6)) {
		command->type = CONTROL_NETWORK_MODE;
		/* Party accent defaults to FULL, not 0. An omitted argument
		 * means "the caller said nothing about the accent", and the
		 * historical render is full strength — defaulting to 0 would
		 * let any bare `mode party` silently switch the accent off
		 * fleet-wide. 0 is only ever set by asking for it. */
		command->value6 = 100;
		if (strcmp(arguments[0], "normal") == 0) {
			command->value = 0;
		} else if (strcmp(arguments[0], "party") == 0) {
			command->value = 1;
		} else if (strcmp(arguments[0], "ultra") == 0) {
			command->value = 2;
		} else {
			return CONTROL_PARSE_BAD_ARGUMENTS;
		}
		if (!parse_integer(arguments[1], 0, UINT32_MAX,
				   &command->value2)) {
			return CONTROL_PARSE_OUT_OF_RANGE;
		}
		if (argument_count >= 3) {
			/* effect period in ms, multiples of 10 (0 = no
			 * effect); the floor mirrors
			 * NETWORK_MODE_BLINK_MIN_CS so nothing remotely
			 * settable approaches the photosensitive band */
			if (!parse_integer(arguments[2], 0, 2550,
					   &command->value3) ||
			    command->value3 % 10 != 0 ||
			    (command->value3 != 0 && command->value3 < 130)) {
				return CONTROL_PARSE_OUT_OF_RANGE;
			}
			command->value3 /= 10;   /* ms -> centiseconds */
		}
		if (argument_count >= 4) {
			/* effect duty in percent (on-fraction for hard,
			 * brightness floor for fade); 100 = solid */
			if (!parse_integer(arguments[3], 20, 100,
					   &command->value4) ||
			    (command->value4 > 80 && command->value4 != 100)) {
				return CONTROL_PARSE_OUT_OF_RANGE;
			}
		}
		if (argument_count >= 5) {
			if (strcmp(arguments[4], "hard") == 0) {
				command->value5 = 0;
			} else if (strcmp(arguments[4], "fade") == 0) {
				command->value5 = 1;
			} else {
				return CONTROL_PARSE_BAD_ARGUMENTS;
			}
		}
		if (argument_count == 6) {
			/* party accent strength, percent; 0 = off */
			if (!parse_integer(arguments[5], 0, 100,
					   &command->value6)) {
				return CONTROL_PARSE_OUT_OF_RANGE;
			}
		}
	} else if (strcmp(name, "heal") == 0 && argument_count == 1 &&
		   strcmp(arguments[0], "fire") == 0) {
		command->type = CONTROL_HEAL_SIM;
		command->value = CONTROL_HEAL_SIM_FIRE;
	} else if (strcmp(name, "heal") == 0 && argument_count == 1 &&
		   strcmp(arguments[0], "clear") == 0) {
		command->type = CONTROL_HEAL_SIM;
		command->value = CONTROL_HEAL_SIM_CLEAR;
	} else if (strcmp(name, "heal") == 0 && argument_count == 2 &&
		   strcmp(arguments[0], "alone") == 0) {
		command->type = CONTROL_HEAL_SIM;
		command->value = CONTROL_HEAL_SIM_ALONE;
		if (!parse_integer(arguments[1], 0, 1, &command->value2)) {
			return CONTROL_PARSE_OUT_OF_RANGE;
		}
	} else if (strcmp(name, "heal") == 0 && argument_count == 2 &&
		   strcmp(arguments[0], "attempts") == 0) {
		command->type = CONTROL_HEAL_SIM;
		command->value = CONTROL_HEAL_SIM_ATTEMPTS;
		if (!parse_integer(arguments[1], 0, 255, &command->value2)) {
			return CONTROL_PARSE_OUT_OF_RANGE;
		}
	} else {
		return command_name_known(name) ? CONTROL_PARSE_BAD_ARGUMENTS
						: CONTROL_PARSE_UNKNOWN_COMMAND;
	}

	return CONTROL_PARSE_OK;
}

bool control_command_requires_auth(const struct control_command *command)
{
	return !command->query;
}

const char *control_parse_result_name(enum control_parse_result result)
{
	switch (result) {
	case CONTROL_PARSE_OK: return "ok";
	case CONTROL_PARSE_EMPTY: return "empty";
	case CONTROL_PARSE_TOO_LONG: return "too_long";
	case CONTROL_PARSE_INVALID_TEXT: return "invalid_text";
	case CONTROL_PARSE_BAD_ENVELOPE: return "bad_envelope";
	case CONTROL_PARSE_UNKNOWN_COMMAND: return "unknown_command";
	case CONTROL_PARSE_BAD_ARGUMENTS: return "bad_arguments";
	case CONTROL_PARSE_OUT_OF_RANGE: return "out_of_range";
	default: return "parse_error";
	}
}

bool control_format(char *buffer, size_t capacity, const char *format, ...)
{
	if (buffer == NULL || capacity == 0) {
		return false;
	}
	va_list arguments;

	va_start(arguments, format);
	int length = vsnprintf(buffer, capacity, format, arguments);
	va_end(arguments);
	if (length < 0 || (size_t)length >= capacity) {
		buffer[0] = '\0';
		return false;
	}
	return true;
}

bool control_encode_reply(char *buffer, size_t capacity,
			  const struct control_command *command,
			  const char *payload)
{
	return command->authenticated
		       ? control_format(buffer, capacity, "v2 %016llx ok %s",
					(unsigned long long)command->sequence, payload)
	       : command->versioned
		       ? control_format(buffer, capacity, "v1 %s ok %s",
					command->request_id, payload)
		       : control_format(buffer, capacity, "%s", payload);
}

bool control_encode_error(char *buffer, size_t capacity,
			  const struct control_command *command,
			  const char *error)
{
	return command->authenticated
		       ? control_format(buffer, capacity, "v2 %016llx error %s",
					(unsigned long long)command->sequence, error)
	       : command->versioned
		       ? control_format(buffer, capacity, "v1 %s error %s",
					command->request_id, error)
		       : control_format(buffer, capacity, "error=%s", error);
}

bool control_encode_status(char *buffer, size_t capacity,
			   const struct control_status *status)
{
	return control_format(
		buffer, capacity,
		"%s up=%llds state=%s theta=%lldms ppm=%d nbr=%d/%d/%d "
		"age=%lldms lapse=%u sched=%s quiet=%us mode=%s/%u infra=%d part=%08x usb=%d usbquiet=%d "
		"reset=%08x/%s "
		"flash=%lld pat=%d budget=%u solo=%s alwayson=%s bright=%d%% layout=%s leds=%d power=%s sleep=%s frames=%u "
		/* bat is millivolts at the sense pin, or -1 when unavailable.
		 * Emitted bare so boot_test.parse_status()'s numeric regex
		 * yields an int; an "mv" suffix would make it a string. */
		"boot=%u/%s build=%s auth=%s bat=%d",
		status->name, (long long)status->uptime_s, status->app_state,
		(long long)status->theta_ms, status->rate_ppm,
		status->locked_peers, status->fresh_peers, status->tracked_peers,
		(long long)status->oldest_age_ms, status->freshness_lapses,
		status->sync_profile, status->ota_quiet_s,
		status->network_mode, status->mode_priority,
		status->infrastructure_ok ? 1 : 0, status->partition_id,
		status->usb_present ? 1 : 0,
		status->usb_quiet ? 1 : 0,
		status->reset_cause, status->reset_reason,
		(long long)status->flash_period_ms, status->pattern,
		status->led_budget, status->solo_mode ? "on" : "off",
		status->always_on ? "on" : "off",
		status->brightness_pct, status->led_layout, status->led_count,
		status->led_power, status->sleep_armed ? "armed" : "idle",
		status->frames, status->boot_sequence, status->boot_stage, status->build_id,
		status->auth_ready ? "ready" : "unprovisioned",
		status->battery_mv);
}
