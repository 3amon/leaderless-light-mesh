#include <errno.h>
#include <string.h>
#include <zephyr/settings/settings.h>
#include <zephyr/ztest.h>

#include "boot_status.h"
#include "control_auth.h"
#include "led_config.h"
#include "led_layout.h"
#include "led_power_mode.h"
#include "network_mode.h"
#include "ota_policy.h"

/* network_mode.c's transmitter is intentionally outside this persistence
 * test; the pure wire/order contract has its own host test. */
void sync_send_aux(const uint8_t *wire, size_t length)
{
	ARG_UNUSED(wire);
	ARG_UNUSED(length);
}

static const char *const auth_message_41 =
	"v2 0000000000000029 scramble -600000";
static const char *const auth_message_42 =
	"v2 000000000000002a scramble -600000";
static const char *const auth_message_42_other =
	"v2 000000000000002a reboot";
static const char *const auth_message_43 =
	"v2 000000000000002b reboot";
static const uint8_t auth_tag_41[CONTROL_AUTH_TAG_BYTES] = {
	0x8a, 0xb2, 0x24, 0x04, 0xcb, 0x5d, 0x82, 0xf1, 0x38, 0xa1, 0xfb, 0xd0,
};
static const uint8_t auth_tag_42[CONTROL_AUTH_TAG_BYTES] = {
	0x95, 0x5b, 0xb2, 0xd1, 0x23, 0x4e, 0x91, 0xc4, 0xe1, 0xd5, 0x93, 0x2e,
};
static const uint8_t auth_tag_42_other[CONTROL_AUTH_TAG_BYTES] = {
	0xfc, 0x9b, 0xe7, 0xa9, 0x27, 0x5c, 0x8d, 0x27, 0xb1, 0xd5, 0x49, 0x73,
};
static const uint8_t auth_tag_43[CONTROL_AUTH_TAG_BYTES] = {
	0xb4, 0x3e, 0x48, 0x81, 0x92, 0x43, 0xc8, 0xfe, 0xcd, 0x82, 0x14, 0x53,
};
static const uint8_t auth_response_tag[CONTROL_AUTH_TAG_BYTES] = {
	0x57, 0xe0, 0x16, 0xb6, 0x99, 0xb5, 0x4a, 0x63, 0x8c, 0xea, 0xcf, 0xfe,
};

ZTEST(thread_ring_boot_ota, test_led_layout_defaults_changes_and_persists)
{
	zassert_ok(settings_subsys_init());
	int rc = settings_delete("ringcfg/led");

	zassert_true(rc == 0 || rc == -ENOENT);
	zassert_ok(led_config_init());
	zassert_equal(led_config_layout(), LED_LAYOUT_RING16);
	zassert_equal(led_layout_pixel_count(led_config_layout()), 16);
	zassert_equal(led_layout_default_brightness(led_config_layout()), 45);

	zassert_ok(led_config_set_layout(LED_LAYOUT_JEWEL7));
	zassert_equal(led_config_layout(), LED_LAYOUT_JEWEL7);
	zassert_equal(led_layout_pixel_count(led_config_layout()), 7);
	zassert_equal(led_layout_default_brightness(led_config_layout()), 100);
	zassert_true(led_layout_uses_usb_quiet_display(led_config_layout()));

	zassert_ok(led_config_set_layout(LED_LAYOUT_ROPE100));
	zassert_equal(led_config_layout(), LED_LAYOUT_ROPE100);
	zassert_equal(led_layout_pixel_count(led_config_layout()), 100);
	zassert_equal(led_layout_default_brightness(led_config_layout()), 100);
	zassert_false(led_layout_uses_usb_quiet_display(led_config_layout()));

	/* Reload the real settings record, approximating an application reboot. */
	zassert_ok(led_config_init());
	zassert_equal(led_config_layout(), LED_LAYOUT_ROPE100);
	zassert_ok(led_config_set_layout(LED_LAYOUT_ROPE100));
	zassert_equal(led_config_set_layout((enum led_layout)3), -EINVAL);
}

ZTEST(thread_ring_boot_ota, test_led_power_defaults_changes_and_persists)
{
	zassert_ok(settings_subsys_init());
	int rc = settings_delete("ringpower/mode");

	zassert_true(rc == 0 || rc == -ENOENT);
	zassert_ok(led_power_mode_init());
	zassert_equal(led_power_mode_get(), LED_POWER_MODULE_VCC);
	zassert_equal(strcmp(led_power_mode_name(led_power_mode_get()), "module-vcc"), 0);

	zassert_ok(led_power_mode_set(LED_POWER_CARRIER));
	zassert_equal(led_power_mode_get(), LED_POWER_CARRIER);
	zassert_equal(strcmp(led_power_mode_name(led_power_mode_get()), "carrier"), 0);

	/* Reload the real settings record, approximating an application reboot. */
	zassert_ok(led_power_mode_init());
	zassert_equal(led_power_mode_get(), LED_POWER_CARRIER);
	zassert_ok(led_power_mode_set(LED_POWER_CARRIER));
	zassert_equal(led_power_mode_set((enum led_power_mode)2), -EINVAL);
}

ZTEST(thread_ring_boot_ota, test_network_mode_priority_and_persistence)
{
	zassert_ok(settings_subsys_init());
	int rc = settings_delete("fleetmode/state");

	zassert_true(rc == 0 || rc == -ENOENT);
	zassert_ok(network_mode_init(0x12345678U));
	struct network_mode_value value = network_mode_get();

	zassert_equal(value.mode, NETWORK_MODE_NORMAL);
	zassert_equal(value.priority, 0);
	zassert_equal(network_mode_set(NETWORK_MODE_PARTY, 10),
		      NETWORK_MODE_ADOPTED);
	zassert_equal(network_mode_set(NETWORK_MODE_NORMAL, 9),
		      NETWORK_MODE_REJECTED);

	/* Reload the CRC-protected settings record as on an application reboot. */
	zassert_ok(network_mode_init(0x12345678U));
	value = network_mode_get();
	zassert_equal(value.mode, NETWORK_MODE_PARTY);
	zassert_equal(value.priority, 10);
	zassert_equal(network_mode_set(NETWORK_MODE_NORMAL, 20),
		      NETWORK_MODE_ADOPTED);
	zassert_equal(network_mode_set(NETWORK_MODE_PARTY, 20),
		      NETWORK_MODE_REJECTED);
	zassert_ok(network_mode_init(0x12345678U));
	value = network_mode_get();
	zassert_equal(value.mode, NETWORK_MODE_NORMAL);
	zassert_equal(value.priority, 20);
}

ZTEST(thread_ring_boot_ota, test_control_auth_persists_replay_and_idempotency)
{
	zassert_ok(settings_subsys_init());
	int rc = settings_delete("ringauth/state");

	zassert_true(rc == 0 || rc == -ENOENT);
	zassert_ok(control_auth_init());
	zassert_false(control_auth_ready());
	zassert_equal(control_auth_sequence(), 0);
	zassert_equal(control_auth_authorize(auth_message_42,
					     strlen(auth_message_42), 42,
					     auth_tag_42),
		      CONTROL_AUTH_UNPROVISIONED);
	zassert_ok(control_auth_provision_hex(
		"000102030405060708090a0b0c0d0e0f"
		"101112131415161718191a1b1c1d1e1f"));
	zassert_true(control_auth_ready());
	uint8_t response_tag[CONTROL_AUTH_TAG_BYTES];
	const char *response = "v2 000000000000002a ok rebooting";

	zassert_ok(control_auth_sign(response, strlen(response), response_tag));
	zassert_mem_equal(response_tag, auth_response_tag, sizeof(response_tag));
	zassert_equal(control_auth_authorize(auth_message_42,
					     strlen(auth_message_42), 42,
					     auth_tag_42),
		      CONTROL_AUTH_ACCEPT_NEW);
	zassert_equal(control_auth_sequence(), 42);
	zassert_equal(control_auth_authorize(auth_message_42,
					     strlen(auth_message_42), 42,
					     auth_tag_42),
		      CONTROL_AUTH_ACCEPT_DUPLICATE);
	zassert_equal(control_auth_authorize(auth_message_42_other,
					     strlen(auth_message_42_other), 42,
					     auth_tag_42_other),
		      CONTROL_AUTH_REPLAY);
	zassert_equal(control_auth_authorize(auth_message_41,
					     strlen(auth_message_41), 41,
					     auth_tag_41),
		      CONTROL_AUTH_REPLAY);

	/* Reload the real settings record, approximating an application restart. */
	zassert_ok(control_auth_init());
	zassert_true(control_auth_ready());
	zassert_equal(control_auth_sequence(), 42);
	zassert_equal(control_auth_authorize(auth_message_42,
					     strlen(auth_message_42), 42,
					     auth_tag_42),
		      CONTROL_AUTH_ACCEPT_DUPLICATE);
	zassert_equal(control_auth_authorize(auth_message_43,
					     strlen(auth_message_43), 43,
					     auth_tag_43),
		      CONTROL_AUTH_ACCEPT_NEW);
}

ZTEST(thread_ring_boot_ota, test_persistent_boot_and_confirmation_lifecycle)
{
	char status[384];
	struct ota_policy policy;
	struct ota_policy_observation observation = {
		.now_ms = 1000,
		.rollback_timeout_ms = 300000,
		.attached = true,
		.infrastructure_visible = false,
		.image_confirmed = false,
	};

	zassert_ok(boot_status_init(0x12, 0x03, 0xd6, 0xa7));
	zassert_equal(boot_status_sequence(), 1);
	zassert_equal(boot_status_stage(), BOOT_STAGE_ENTRY);

	ota_policy_init(&policy);
	struct ota_policy_decision decision =
		ota_policy_update(&policy, &observation);

	zassert_true(decision.record_attached);
	zassert_false(decision.infrastructure_ok);
	zassert_false(decision.confirmation_due);
	zassert_ok(boot_status_record(BOOT_STAGE_ATTACHED));

	observation.infrastructure_visible = true;
	decision = ota_policy_update(&policy, &observation);
	zassert_true(decision.infrastructure_ok);
	zassert_true(decision.record_infrastructure);
	zassert_true(decision.confirmation_due);
	zassert_true(policy.ever_infrastructure);
	zassert_ok(boot_status_record(BOOT_STAGE_INFRASTRUCTURE));

	observation.now_ms += OTA_CONFIRM_RETRY_MS - 1;
	decision = ota_policy_update(&policy, &observation);
	zassert_false(decision.confirmation_due);
	observation.image_confirmed = true;
	decision = ota_policy_update(&policy, &observation);
	zassert_true(decision.record_confirmed);
	zassert_ok(boot_status_record(BOOT_STAGE_CONFIRMED));

	zassert_true(boot_status_format(status, sizeof(status)) > 0);
	zassert_not_null(strstr(status, "boot=1 stage=confirmed"));
	zassert_not_null(strstr(status, "mcuboot=d6/a7"));
	zassert_not_null(strstr(status, "1:entry@0"));
	zassert_not_null(strstr(status, "1:attached@"));
	zassert_not_null(strstr(status, "1:infra@"));
	zassert_not_null(strstr(status, "1:confirmed@"));
	zassert_equal(boot_status_format(status, 8), -ENOSPC);

	/* A second initialization exercises the real settings load and CRC path,
	 * approximating an application restart without physical flash hardware. */
	zassert_ok(boot_status_init(0x04, 0x05, 0xdd, 0x80));
	zassert_equal(boot_status_sequence(), 2);
	zassert_equal(boot_status_stage(), BOOT_STAGE_ENTRY);
	zassert_true(boot_status_format(status, sizeof(status)) > 0);
	zassert_not_null(strstr(status, "boot=2 stage=entry"));
	zassert_not_null(strstr(status, "1:confirmed@"));
	zassert_not_null(strstr(status, "2:entry@0"));
}

ZTEST(thread_ring_boot_ota, test_unconfirmed_deadline_is_strict)
{
	struct ota_policy policy;
	struct ota_policy_observation observation = {
		.now_ms = 300000,
		.rollback_timeout_ms = 300000,
		.attached = true,
		.infrastructure_visible = true,
		.image_confirmed = false,
	};

	ota_policy_init(&policy);
	struct ota_policy_decision decision =
		ota_policy_update(&policy, &observation);

	zassert_false(decision.rollback_due);
	zassert_true(decision.confirmation_due);
	observation.now_ms++;
	decision = ota_policy_update(&policy, &observation);
	zassert_true(decision.rollback_due);
	zassert_false(decision.confirmation_due);
}

ZTEST_SUITE(thread_ring_boot_ota, NULL, NULL, NULL, NULL, NULL);
