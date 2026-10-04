#ifndef THREAD_RING_APP_STATE_H_
#define THREAD_RING_APP_STATE_H_

#include <stdbool.h>
#include <stdint.h>

#define APP_STATE_BOOT_STATUS_MS 2400

/* A border router is NOT part of the deployed topology — wearers form ad-hoc
 * meshes with no infrastructure at all. Rendering therefore never consults
 * border-router visibility; a node that is attached and out of DFU is HEALTHY
 * and shows its normal solo/group composite. Infrastructure visibility still
 * gates OTA image confirmation (see ota_policy.h), which is a genuinely
 * different question: "is this image remotely recoverable?" */
enum app_state {
	APP_STATE_BOOTING = 0,
	APP_STATE_SEARCHING,
	APP_STATE_HEALTHY,
	APP_STATE_DFU,
	APP_STATE_RECOVERY,
};

struct app_state_observation {
	int64_t now_ms;
	bool recovery_boot;
	bool attached;
	bool dfu_active;
};

struct app_state_machine {
	enum app_state current;
	int64_t entered_ms;
	uint32_t transitions;
};

struct app_state_transition {
	enum app_state previous;
	enum app_state current;
	int64_t entered_ms;
	bool changed;
};

enum app_state app_state_select(const struct app_state_observation *observation);
void app_state_init(struct app_state_machine *machine,
		    const struct app_state_observation *observation);
struct app_state_transition app_state_update(
	struct app_state_machine *machine,
	const struct app_state_observation *observation);
const char *app_state_name(enum app_state state);

#endif /* THREAD_RING_APP_STATE_H_ */
