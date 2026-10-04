#include "sync_schedule.h"

#include <limits.h>

static int64_t deadline_after(int64_t now_ms, int64_t delay_ms)
{
	return now_ms > INT64_MAX - delay_ms ? INT64_MAX : now_ms + delay_ms;
}

void sync_compat_policy_init(struct sync_compat_policy *policy,
			     int64_t now_ms)
{
	policy->compat_until_ms = deadline_after(now_ms, SYNC_COMPAT_QUIET_MS);
}

void sync_compat_policy_note_wire_version(struct sync_compat_policy *policy,
					  uint8_t wire_version,
					  int64_t now_ms)
{
	if (wire_version >= 3U) {
		return;
	}
	int64_t deadline = deadline_after(now_ms, SYNC_COMPAT_QUIET_MS);

	if (deadline > policy->compat_until_ms) {
		policy->compat_until_ms = deadline;
	}
}

enum sync_schedule_profile sync_compat_policy_profile(
	const struct sync_compat_policy *policy, int64_t now_ms)
{
	return now_ms < policy->compat_until_ms ? SYNC_SCHEDULE_COMPAT
						 : SYNC_SCHEDULE_SCALABLE;
}

struct sync_schedule_range sync_schedule_range(
	enum sync_schedule_profile profile, enum sync_schedule_path path)
{
	if (profile == SYNC_SCHEDULE_COMPAT) {
		/* This is the intended old ±10% range. The old expression only
		 * produced 450, 451, or 452 ms. */
		return (struct sync_schedule_range){450U, 550U};
	}
	if (path == SYNC_SCHEDULE_LOCAL) {
		return (struct sync_schedule_range){1800U, 2600U};
	}
	return (struct sync_schedule_range){8000U, 12000U};
}

uint32_t sync_schedule_delay_ms(enum sync_schedule_profile profile,
				enum sync_schedule_path path,
				uint32_t random_value)
{
	struct sync_schedule_range range = sync_schedule_range(profile, path);
	uint32_t width = range.maximum_ms - range.minimum_ms + 1U;

	return range.minimum_ms + random_value % width;
}

uint32_t sync_schedule_change_delay_ms(uint32_t random_value)
{
	/* Spread a fleet-wide view transition instead of creating a multicast
	 * wall. The steady realm schedule remains the retry if this is lost. */
	return 1000U + random_value % 4001U;
}

const char *sync_schedule_profile_name(enum sync_schedule_profile profile)
{
	return profile == SYNC_SCHEDULE_COMPAT ? "compat" : "scalable";
}
