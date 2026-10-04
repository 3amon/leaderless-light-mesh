#include "ota_quiet.h"

void ota_quiet_init(struct ota_quiet_policy *policy)
{
	policy->until_ms = 0;
}

void ota_quiet_set(struct ota_quiet_policy *policy, int64_t now_ms,
		   uint32_t duration_s)
{
	policy->until_ms = duration_s == 0U
			   ? 0
			   : now_ms + (int64_t)duration_s * 1000;
}

int64_t ota_quiet_remaining_ms(const struct ota_quiet_policy *policy,
			       int64_t now_ms)
{
	return policy->until_ms > now_ms ? policy->until_ms - now_ms : 0;
}

uint32_t ota_quiet_remaining_s(const struct ota_quiet_policy *policy,
			       int64_t now_ms)
{
	int64_t remaining = ota_quiet_remaining_ms(policy, now_ms);

	return remaining == 0 ? 0U : (uint32_t)((remaining + 999) / 1000);
}

bool ota_quiet_active(const struct ota_quiet_policy *policy, int64_t now_ms)
{
	return ota_quiet_remaining_ms(policy, now_ms) > 0;
}
