/* Pure scheduling and rolling-migration policy for gossip beacons. */

#ifndef THREAD_RING_SYNC_SCHEDULE_H_
#define THREAD_RING_SYNC_SCHEDULE_H_

#include <stdint.h>

enum sync_schedule_profile {
	SYNC_SCHEDULE_COMPAT,
	SYNC_SCHEDULE_SCALABLE,
};

enum sync_schedule_path {
	SYNC_SCHEDULE_LOCAL,
	SYNC_SCHEDULE_REALM,
};

struct sync_schedule_range {
	uint32_t minimum_ms;
	uint32_t maximum_ms;
};

/* A v0.1.63-or-older beacon keeps the whole rolling fleet on the corrected
 * legacy cadence. v0.1.64 transmits only v3, so legacy traffic unambiguously
 * means that an older node is still present. */
#define SYNC_COMPAT_QUIET_MS 120000LL

/* Freshness is deliberately much longer than the scalable cadence. A brief
 * burst loss must not turn an established group back into a collection of
 * solo flowers. */
#define SYNC_DIRECT_FRESH_US  12000000LL
#define SYNC_OVERALL_FRESH_US 45000000LL
#define SYNC_PEER_TIMEOUT_US  90000000LL

struct sync_compat_policy {
	int64_t compat_until_ms;
};

void sync_compat_policy_init(struct sync_compat_policy *policy,
			     int64_t now_ms);
void sync_compat_policy_note_wire_version(struct sync_compat_policy *policy,
					  uint8_t wire_version,
					  int64_t now_ms);
enum sync_schedule_profile sync_compat_policy_profile(
	const struct sync_compat_policy *policy, int64_t now_ms);

struct sync_schedule_range sync_schedule_range(
	enum sync_schedule_profile profile, enum sync_schedule_path path);
uint32_t sync_schedule_delay_ms(enum sync_schedule_profile profile,
				enum sync_schedule_path path,
				uint32_t random_value);
uint32_t sync_schedule_change_delay_ms(uint32_t random_value);
const char *sync_schedule_profile_name(enum sync_schedule_profile profile);

#endif
