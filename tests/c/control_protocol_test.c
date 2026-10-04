#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "control_protocol.h"

#define PATTERNS 7

static struct control_command parse_ok(const char *text)
{
	struct control_command command;

	assert(control_parse(text, strlen(text), PATTERNS, &command) ==
	       CONTROL_PARSE_OK);
	return command;
}

static void parse_fails(const char *text, enum control_parse_result expected)
{
	struct control_command command;
	enum control_parse_result actual =
		control_parse(text, strlen(text), PATTERNS, &command);

	if (actual != expected) {
		fprintf(stderr, "parse %s: expected %s, got %s\n", text,
			control_parse_result_name(expected),
			control_parse_result_name(actual));
	}
	assert(actual == expected);
}

static void test_exact_commands(void)
{
	struct control_command command = parse_ok("status");

	assert(command.type == CONTROL_STATUS && !command.versioned);
	assert(parse_ok("group").type == CONTROL_GROUP);
	assert(parse_ok("boot").type == CONTROL_BOOT);
	assert(parse_ok("auth").type == CONTROL_AUTH_STATUS);
	assert(parse_ok("update").type == CONTROL_UPDATE);
	assert(parse_ok("reboot").type == CONTROL_REBOOT);
	assert(parse_ok("sleep").type == CONTROL_SLEEP);
	assert(parse_ok("hb").type == CONTROL_HEARTBEAT);

	command = parse_ok("flash off");
	assert(command.type == CONTROL_FLASH && command.value == 0);
	assert(parse_ok("flash 200").value == 200);
	assert(parse_ok("flash 60000").value == 60000);
	assert(parse_ok("scramble -600000").value == -600000);
	assert(parse_ok("scramble 600000").value == 600000);
	assert(parse_ok("skew off").value == 0);
	assert(parse_ok("skew -6000").value == -6000);
	assert(parse_ok("skew 6000").value == 6000);
	assert(parse_ok("pattern auto").value == -1);
	assert(parse_ok("pattern 0").value == 0);
	assert(parse_ok("pattern 6").value == 6);
	assert(parse_ok("bright 1").value == 1);
	assert(parse_ok("bright 100").value == 100);
	command = parse_ok("layout");
	assert(command.type == CONTROL_LED_LAYOUT && command.query);
	command = parse_ok("layout ring16");
	assert(command.type == CONTROL_LED_LAYOUT && command.value == 16 &&
	       !command.query);
	assert(parse_ok("layout jewel7").value == 7);
	assert(parse_ok("layout rope100").value == 100);
	command = parse_ok("power");
	assert(command.type == CONTROL_LED_POWER && command.query);
	command = parse_ok("power carrier");
	assert(command.type == CONTROL_LED_POWER && command.value == 0 &&
	       !command.query);
	assert(parse_ok("power module-vcc").value == 1);
	assert(parse_ok("gossip local").value == CONTROL_GOSSIP_LOCAL);
	assert(parse_ok("gossip realm").value == CONTROL_GOSSIP_REALM);
	assert(parse_ok("gossip both").value == CONTROL_GOSSIP_BOTH);
	command = parse_ok("quiet");
	assert(command.type == CONTROL_OTA_QUIET && command.query);
	assert(!control_command_requires_auth(&command));
	command = parse_ok("quiet off");
	assert(command.type == CONTROL_OTA_QUIET && command.value == 0 &&
	       !command.query);
	assert(control_command_requires_auth(&command));
	assert(parse_ok("quiet 30").value == 30);
	assert(parse_ok("quiet 14400").value == 14400);
	command = parse_ok("mode");
	assert(command.type == CONTROL_NETWORK_MODE && command.query);
	assert(!control_command_requires_auth(&command));
	command = parse_ok("mode party 10");
	assert(command.type == CONTROL_NETWORK_MODE && command.value == 1 &&
	       command.value2 == 10 && !command.query);
	assert(control_command_requires_auth(&command));
	command = parse_ok("mode ultra 11");
	assert(command.type == CONTROL_NETWORK_MODE && command.value == 2 &&
	       command.value2 == 11 && !command.query);
	assert(command.value3 == 0);
	assert(control_command_requires_auth(&command));
	command = parse_ok("mode ultra 11 320");
	assert(command.type == CONTROL_NETWORK_MODE && command.value == 2 &&
	       command.value2 == 11 && command.value3 == 32);
	assert(command.value4 == 0);
	command = parse_ok("mode ultra 11 320 70");
	assert(command.type == CONTROL_NETWORK_MODE && command.value == 2 &&
	       command.value2 == 11 && command.value3 == 32 &&
	       command.value4 == 70);
	assert(parse_ok("mode ultra 11 320 100").value4 == 100);
	/* the full long-form command fits the 96-byte datagram even with a
	 * v2 auth envelope: "normal" + 4-digit blink + duty + "fade" was
	 * exactly one byte over the old 71-byte cap (a real field paper cut) */
	command = parse_ok(
		"v2 0000000000000016 mode normal 4294967295 2000 30 fade"
		" 00112233445566778899aabb");
	assert(command.type == CONTROL_NETWORK_MODE && command.value == 0 &&
	       command.value3 == 200 && command.value4 == 30 &&
	       command.value5 == 1);
	command = parse_ok("mode party 12 1000 30 fade");
	assert(command.value == 1 && command.value3 == 100 &&
	       command.value4 == 30 && command.value5 == 1);
	command = parse_ok("mode normal 13 500 50 hard");
	assert(command.value == 0 && command.value5 == 0);
	assert(parse_ok("mode ultra 14 0").value3 == 0);   /* no effect */
	command = parse_ok("mode normal 4294967295");
	assert(command.value == 0 && command.value2 == UINT32_MAX);
}

static void test_authenticated_envelope(void)
{
	struct control_command command = parse_ok(
		"v2 000000000000002a scramble -600000 00112233445566778899aabb");
	const uint8_t expected_tag[CONTROL_AUTH_TAG_BYTES] = {
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
		0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
	};

	assert(command.versioned && command.authenticated);
	assert(command.sequence == 42);
	assert(command.signed_length == strlen("v2 000000000000002a scramble -600000"));
	assert(memcmp(command.auth_tag, expected_tag, sizeof(expected_tag)) == 0);
	assert(command.type == CONTROL_SCRAMBLE && command.value == -600000);
	assert(control_command_requires_auth(&command));
	command = parse_ok("status");
	assert(!control_command_requires_auth(&command));

	parse_fails("v2 2a reboot 00112233445566778899aabb",
		    CONTROL_PARSE_BAD_ENVELOPE);
	parse_fails("v2 000000000000002A reboot 00112233445566778899aabb",
		    CONTROL_PARSE_BAD_ENVELOPE);
	parse_fails("v2 000000000000002a reboot 00112233445566778899aabB",
		    CONTROL_PARSE_BAD_ENVELOPE);
	parse_fails("v2 000000000000002a reboot 0011",
		    CONTROL_PARSE_BAD_ENVELOPE);
}

static void test_versioned_envelope(void)
{
	struct control_command command = parse_ok("v1 pi-01 status");

	assert(command.versioned);
	assert(strcmp(command.request_id, "pi-01") == 0);
	assert(command.type == CONTROL_STATUS);

	command = parse_ok("v1 ota_123 update");
	assert(command.versioned && command.type == CONTROL_UPDATE);
	assert(strcmp(command.request_id, "ota_123") == 0);

	parse_fails("v1", CONTROL_PARSE_BAD_ENVELOPE);
	parse_fails("v1 only-id", CONTROL_PARSE_BAD_ENVELOPE);
	parse_fails("v1 bad.id status", CONTROL_PARSE_BAD_ENVELOPE);
	parse_fails("v1 12345678901234567 status", CONTROL_PARSE_BAD_ENVELOPE);

	assert(control_parse("v1 req-8 status extra", 21, PATTERNS, &command) ==
	       CONTROL_PARSE_BAD_ARGUMENTS);
	assert(command.versioned && strcmp(command.request_id, "req-8") == 0);
}

static void test_rejects_ambiguous_and_invalid_input(void)
{
	parse_fails("", CONTROL_PARSE_EMPTY);
	parse_fails("statusjunk", CONTROL_PARSE_UNKNOWN_COMMAND);
	parse_fails("reboot-later", CONTROL_PARSE_UNKNOWN_COMMAND);
	parse_fails("update now", CONTROL_PARSE_BAD_ARGUMENTS);
	parse_fails("sleep now", CONTROL_PARSE_BAD_ARGUMENTS);
	parse_fails("flash", CONTROL_PARSE_BAD_ARGUMENTS);
	parse_fails("flash office", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("skew cutoff", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("pattern automatic", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("bright 50pct", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("gossip local-realm", CONTROL_PARSE_BAD_ARGUMENTS);
	parse_fails("quiet 29", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("quiet 14401", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("quiet forever", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("quiet off now", CONTROL_PARSE_BAD_ARGUMENTS);
	parse_fails("mode dance 10", CONTROL_PARSE_BAD_ARGUMENTS);
	parse_fails("mode party", CONTROL_PARSE_BAD_ARGUMENTS);
	parse_fails("mode ultra", CONTROL_PARSE_BAD_ARGUMENTS);
	parse_fails("mode ultra 11 90", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("mode ultra 11 325", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("mode ultra 11 2560", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("mode ultra 11 320 19", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("mode ultra 11 320 81", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("mode ultra 11 320 99", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("mode ultra 11 120", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("mode party 12 1000 30 wobble", CONTROL_PARSE_BAD_ARGUMENTS);
	/* a 6th argument is now the party accent, so "extra" is a bad
	 * integer rather than an unexpected argument; 7 is still too many */
	parse_fails("mode ultra 11 320 70 fade extra", CONTROL_PARSE_OUT_OF_RANGE);
	assert(parse_ok("solo").type == CONTROL_SOLO_MODE);
	assert(parse_ok("solo").query);
	assert(parse_ok("solo on").type == CONTROL_SOLO_MODE);
	assert(parse_ok("solo on").value == 1);
	assert(parse_ok("solo off").value == 0);
	parse_fails("solo maybe", CONTROL_PARSE_BAD_ARGUMENTS);
	assert(parse_ok("always-on").type == CONTROL_ALWAYS_ON);
	assert(parse_ok("always-on").query);
	assert(parse_ok("always-on on").type == CONTROL_ALWAYS_ON);
	assert(parse_ok("always-on on").value == 1);
	assert(parse_ok("always-on off").value == 0);
	parse_fails("always-on sometimes", CONTROL_PARSE_BAD_ARGUMENTS);
	parse_fails("mode ultra 11 320 70 fade 0 extra",
		    CONTROL_PARSE_BAD_ARGUMENTS);
	parse_fails("mode ultra 11 320 70 fade 101",
		    CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("mode ultra 11 320 70 fade -1", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("mode party -1", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("mode normal 4294967296", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("thresh", CONTROL_PARSE_UNKNOWN_COMMAND);
	parse_fails("thresh -72 -82", CONTROL_PARSE_UNKNOWN_COMMAND);
	parse_fails("status ", CONTROL_PARSE_BAD_ARGUMENTS);
	parse_fails(" status", CONTROL_PARSE_BAD_ARGUMENTS);
	parse_fails("flash  2000", CONTROL_PARSE_BAD_ARGUMENTS);
	parse_fails("flash\t2000", CONTROL_PARSE_INVALID_TEXT);
	parse_fails("flash 199", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("flash 60001", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("scramble 999999999999999999999999999999999999999",
		    CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("pattern 7", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("pattern -1", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("bright 0", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("bright 101", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("bright +50", CONTROL_PARSE_OUT_OF_RANGE);
	parse_fails("layout jewel", CONTROL_PARSE_BAD_ARGUMENTS);
	parse_fails("layout 7", CONTROL_PARSE_BAD_ARGUMENTS);
	parse_fails("layout jewel7 extra", CONTROL_PARSE_BAD_ARGUMENTS);
	parse_fails("power raw", CONTROL_PARSE_BAD_ARGUMENTS);
	parse_fails("power module-vcc extra", CONTROL_PARSE_BAD_ARGUMENTS);

	char embedded_nul[] = {'s', 't', 'a', 't', 'u', 's', '\0', 'x'};
	struct control_command command;

	assert(control_parse(embedded_nul, sizeof(embedded_nul), PATTERNS,
			     &command) == CONTROL_PARSE_INVALID_TEXT);

	char too_long[CONTROL_MAX_DATAGRAM + 1];

	memset(too_long, 'x', sizeof(too_long));
	assert(control_parse(too_long, sizeof(too_long), PATTERNS, &command) ==
	       CONTROL_PARSE_TOO_LONG);
}

static void test_bounded_encoding(void)
{
	char buffer[384];
	struct control_command legacy = parse_ok("status");
	struct control_command versioned = parse_ok("v1 req-7 status");

	assert(control_encode_reply(buffer, sizeof(buffer), &legacy, "payload"));
	assert(strcmp(buffer, "payload") == 0);
	assert(control_encode_reply(buffer, sizeof(buffer), &versioned, "payload"));
	assert(strcmp(buffer, "v1 req-7 ok payload") == 0);
	assert(control_encode_error(buffer, sizeof(buffer), &legacy,
				    "bad_arguments"));
	assert(strcmp(buffer, "error=bad_arguments") == 0);
	assert(control_encode_error(buffer, sizeof(buffer), &versioned,
				    "out_of_range"));
	assert(strcmp(buffer, "v1 req-7 error out_of_range") == 0);
	struct control_command authenticated = parse_ok(
		"v2 000000000000002a reboot 00112233445566778899aabb");

	assert(control_encode_reply(buffer, sizeof(buffer), &authenticated,
				    "rebooting"));
	assert(strcmp(buffer, "v2 000000000000002a ok rebooting") == 0);
	assert(control_encode_error(buffer, sizeof(buffer), &authenticated,
				    "replay"));
	assert(strcmp(buffer, "v2 000000000000002a error replay") == 0);

	char tiny[8] = "changed";

	assert(!control_encode_reply(tiny, sizeof(tiny), &versioned, "payload"));
	assert(tiny[0] == '\0');
	assert(!control_format(tiny, sizeof(tiny), "%s", "12345678"));
	assert(tiny[0] == '\0');
}

static void test_status_encoding(void)
{
	char buffer[384];
	struct control_status status = {
		.name = "opal-reef-tide",
		.uptime_s = 123,
		.app_state = "healthy",
		.theta_ms = -2,
		.rate_ppm = 17,
		.locked_peers = 1,
		.fresh_peers = 1,
		.tracked_peers = 1,
		.oldest_age_ms = 450,
		.freshness_lapses = 2,
		.sync_profile = "scalable",
		.ota_quiet_s = 1799,
		.network_mode = "party",
		.mode_priority = 10,
		.infrastructure_ok = true,
		.usb_present = true,
		.usb_quiet = true,
		.partition_id = 0x1234abcd,
		.reset_cause = 2,
		.reset_reason = "ota",
		.flash_period_ms = 0,
		.pattern = 4,
		.led_budget = 90,
		.solo_mode = false,
		.always_on = true,
		.brightness_pct = 45,
		.led_layout = "ring16",
		.led_count = 16,
		.led_power = "module-vcc",
		.sleep_armed = true,
		.frames = 999,
		.boot_sequence = 31,
		.boot_stage = "confirmed",
		.build_id = "v0.1.22",
		.auth_ready = true,
		.battery_mv = 3700,
	};

	assert(control_encode_status(buffer, sizeof(buffer), &status));
	assert(strcmp(buffer,
		      "opal-reef-tide up=123s state=healthy theta=-2ms ppm=17 "
		      "nbr=1/1/1 age=450ms lapse=2 sched=scalable quiet=1799s mode=party/10 infra=1 part=1234abcd usb=1 usbquiet=1 "
		      "reset=00000002/ota flash=0 pat=4 budget=90 solo=off alwayson=on bright=45% layout=ring16 "
		      "leds=16 power=module-vcc sleep=armed frames=999 "
		      "boot=31/confirmed build=v0.1.22 auth=ready bat=3700") == 0);

	char tiny[32];

	assert(!control_encode_status(tiny, sizeof(tiny), &status));
	assert(tiny[0] == '\0');
}


/* Rejoin-policy test injection. These are mutating (never queries) so the
 * generic auth rule must cover them without a special case. */
static void test_heal_sim_commands(void)
{
	struct control_command command;

	assert(control_parse("heal fire", 9, 8, &command) == CONTROL_PARSE_OK);
	assert(command.type == CONTROL_HEAL_SIM);
	assert(command.value == CONTROL_HEAL_SIM_FIRE);
	assert(!command.query);
	assert(control_command_requires_auth(&command));

	assert(control_parse("heal clear", 10, 8, &command) == CONTROL_PARSE_OK);
	assert(command.value == CONTROL_HEAL_SIM_CLEAR);

	assert(control_parse("heal alone 1", 12, 8, &command) == CONTROL_PARSE_OK);
	assert(command.value == CONTROL_HEAL_SIM_ALONE);
	assert(command.value2 == 1);

	assert(control_parse("heal alone 0", 12, 8, &command) == CONTROL_PARSE_OK);
	assert(command.value2 == 0);

	assert(control_parse("heal attempts 3", 15, 8, &command) == CONTROL_PARSE_OK);
	assert(command.value == CONTROL_HEAL_SIM_ATTEMPTS);
	assert(command.value2 == 3);

	assert(control_parse("heal attempts 255", 17, 8, &command) == CONTROL_PARSE_OK);
	assert(command.value2 == 255);

	/* Out of range and malformed forms must be rejected, not clamped. */
	assert(control_parse("heal alone 2", 12, 8, &command) ==
	       CONTROL_PARSE_OUT_OF_RANGE);
	assert(control_parse("heal attempts 256", 17, 8, &command) ==
	       CONTROL_PARSE_OUT_OF_RANGE);
	assert(control_parse("heal", 4, 8, &command) ==
	       CONTROL_PARSE_BAD_ARGUMENTS);
	assert(control_parse("heal wibble", 11, 8, &command) ==
	       CONTROL_PARSE_BAD_ARGUMENTS);
	assert(control_parse("heal alone", 10, 8, &command) ==
	       CONTROL_PARSE_BAD_ARGUMENTS);
}

int main(void)
{
	test_exact_commands();
	test_versioned_envelope();
	test_authenticated_envelope();
	test_rejects_ambiguous_and_invalid_input();
	test_bounded_encoding();
	test_status_encoding();
	test_heal_sim_commands();
	puts("control protocol tests passed");
	return 0;
}
