/* Time-bounded suppression of background radio traffic during OTA. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define OTA_QUIET_MIN_SECONDS 30
#define OTA_QUIET_MAX_SECONDS 14400

struct ota_quiet_policy {
	int64_t until_ms;
};

void ota_quiet_init(struct ota_quiet_policy *policy);
void ota_quiet_set(struct ota_quiet_policy *policy, int64_t now_ms,
		   uint32_t duration_s);
int64_t ota_quiet_remaining_ms(const struct ota_quiet_policy *policy,
			       int64_t now_ms);
uint32_t ota_quiet_remaining_s(const struct ota_quiet_policy *policy,
			       int64_t now_ms);
bool ota_quiet_active(const struct ota_quiet_policy *policy, int64_t now_ms);
