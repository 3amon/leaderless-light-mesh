#ifndef THREAD_RING_OTA_POLICY_H_
#define THREAD_RING_OTA_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

#define OTA_CONFIRM_RETRY_MS 5000

struct ota_policy {
	int64_t next_confirmation_ms;
	bool ever_infrastructure;
};

struct ota_policy_observation {
	int64_t now_ms;
	int64_t rollback_timeout_ms;
	bool attached;
	bool infrastructure_visible;
	bool image_confirmed;
};

struct ota_policy_decision {
	bool infrastructure_ok;
	bool confirmation_due;
	bool rollback_due;
	bool record_attached;
	bool record_infrastructure;
	bool record_confirmed;
};

void ota_policy_init(struct ota_policy *policy);

/* A Thread role is necessary but insufficient: only OTBR-owned Network Data
 * proves that a candidate can still be managed from infrastructure. Calling
 * update reserves each confirmation attempt at a bounded retry cadence. */
struct ota_policy_decision ota_policy_update(
	struct ota_policy *policy,
	const struct ota_policy_observation *observation);

#endif
