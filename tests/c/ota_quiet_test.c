#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "ota_quiet.h"

int main(void)
{
	struct ota_quiet_policy policy;

	ota_quiet_init(&policy);
	assert(!ota_quiet_active(&policy, 1000));
	assert(ota_quiet_remaining_ms(&policy, 1000) == 0);
	assert(ota_quiet_remaining_s(&policy, 1000) == 0);

	ota_quiet_set(&policy, 1234, 30);
	assert(ota_quiet_active(&policy, 1234));
	assert(ota_quiet_remaining_ms(&policy, 1234) == 30000);
	assert(ota_quiet_remaining_s(&policy, 1234) == 30);
	assert(ota_quiet_remaining_s(&policy, 1235) == 30);
	assert(ota_quiet_remaining_s(&policy, 2234) == 29);
	assert(ota_quiet_active(&policy, 31233));
	assert(!ota_quiet_active(&policy, 31234));

	ota_quiet_set(&policy, 40000, OTA_QUIET_MAX_SECONDS);
	assert(ota_quiet_remaining_s(&policy, 40000) ==
	       OTA_QUIET_MAX_SECONDS);
	ota_quiet_set(&policy, 40001, 0);
	assert(!ota_quiet_active(&policy, 40001));

	puts("OTA quiet policy tests passed");
	return 0;
}
