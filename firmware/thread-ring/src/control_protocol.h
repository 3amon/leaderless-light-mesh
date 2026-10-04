/* Strict, bounded UDP control protocol; no Zephyr dependencies. */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONTROL_MAX_DATAGRAM 96
#define CONTROL_REQUEST_ID_MAX 16
#define CONTROL_AUTH_TAG_BYTES 12
#define CONTROL_AUTH_TAG_HEX_LENGTH (CONTROL_AUTH_TAG_BYTES * 2)

enum control_command_type {
	CONTROL_STATUS,
	CONTROL_GROUP,
	CONTROL_BOOT,
	CONTROL_AUTH_STATUS,
	CONTROL_FLASH,
	CONTROL_SCRAMBLE,
	CONTROL_UPDATE,
	CONTROL_REBOOT,
	CONTROL_SLEEP,
	CONTROL_SKEW,
	CONTROL_PATTERN,
	CONTROL_BRIGHTNESS,
	CONTROL_LED_LAYOUT,
	CONTROL_LED_BUDGET,
	CONTROL_SOLO_MODE,
	CONTROL_ALWAYS_ON,
	CONTROL_LED_POWER,
	CONTROL_HEARTBEAT,
	CONTROL_GOSSIP,
	CONTROL_OTA_QUIET,
	CONTROL_NETWORK_MODE,
	CONTROL_HEAL_SIM,
};

/* Test-only injection into the rejoin policy's INPUTS. The policy itself is
 * untouched and stays pure; only the source of its observation changes, so what
 * runs under test is the shipping decision path rather than a mock of it.
 *
 * ALONE and FIRE are deliberately RAM-only and cleared on every boot: they cause
 * reboots, and an override that survived one would be a remote reboot loop on a
 * ring too busy restarting to be commanded back. At most one injected reboot per
 * command; walking the escalation means re-issuing after each one. ATTEMPTS is
 * safe to persist because by itself it triggers nothing. */
enum control_heal_sim {
	CONTROL_HEAL_SIM_CLEAR,
	CONTROL_HEAL_SIM_ALONE,
	CONTROL_HEAL_SIM_ATTEMPTS,
	CONTROL_HEAL_SIM_FIRE,
};

enum control_gossip_mode {
	CONTROL_GOSSIP_LOCAL,
	CONTROL_GOSSIP_REALM,
	CONTROL_GOSSIP_BOTH,
};

enum control_parse_result {
	CONTROL_PARSE_OK,
	CONTROL_PARSE_EMPTY,
	CONTROL_PARSE_TOO_LONG,
	CONTROL_PARSE_INVALID_TEXT,
	CONTROL_PARSE_BAD_ENVELOPE,
	CONTROL_PARSE_UNKNOWN_COMMAND,
	CONTROL_PARSE_BAD_ARGUMENTS,
	CONTROL_PARSE_OUT_OF_RANGE,
};

struct control_command {
	bool versioned;
	bool authenticated;
	bool query;
	char request_id[CONTROL_REQUEST_ID_MAX + 1];
	uint64_t sequence;
	uint8_t auth_tag[CONTROL_AUTH_TAG_BYTES];
	size_t signed_length;
	enum control_command_type type;
	int64_t value;
	int64_t value2;
	int64_t value3;
	int64_t value4;
	int64_t value5;
	int64_t value6;
};

bool control_command_requires_auth(const struct control_command *command);

enum control_parse_result control_parse(const void *data, size_t length,
					int pattern_count,
					struct control_command *command);
const char *control_parse_result_name(enum control_parse_result result);

bool control_format(char *buffer, size_t capacity, const char *format, ...);
bool control_encode_reply(char *buffer, size_t capacity,
			  const struct control_command *command,
			  const char *payload);
bool control_encode_error(char *buffer, size_t capacity,
			  const struct control_command *command,
			  const char *error);

struct control_status {
	const char *name;
	int64_t uptime_s;
	const char *app_state;
	int64_t theta_ms;
	int rate_ppm;
	int locked_peers;
	int fresh_peers;
	int tracked_peers;
	int64_t oldest_age_ms;
	uint32_t freshness_lapses;
	const char *sync_profile;
	uint32_t ota_quiet_s;
	const char *network_mode;
	uint32_t mode_priority;
	bool infrastructure_ok;
	bool usb_present;
	bool usb_quiet;
	uint32_t partition_id;
	uint32_t reset_cause;
	const char *reset_reason;
	int64_t flash_period_ms;
	int pattern;
	uint32_t led_budget;
	bool solo_mode;
	bool always_on;
	int brightness_pct;
	const char *led_layout;
	int led_count;
	const char *led_power;
	bool sleep_armed;
	uint32_t frames;
	uint32_t boot_sequence;
	const char *boot_stage;
	const char *build_id;
	bool auth_ready;
	/* Voltage at the AIN7 battery-sense pin in millivolts, or -1 when the
	 * ADC is unavailable or the read failed. Deliberately unscaled: it is
	 * not yet confirmed that these SuperMini clones populate a divider. */
	int battery_mv;
};

bool control_encode_status(char *buffer, size_t capacity,
			   const struct control_status *status);
