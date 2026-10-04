#include "app_state.h"

enum app_state app_state_select(const struct app_state_observation *observation)
{
	if (observation->now_ms < APP_STATE_BOOT_STATUS_MS) {
		return observation->recovery_boot ? APP_STATE_RECOVERY
						  : APP_STATE_BOOTING;
	}
	if (observation->dfu_active) {
		return APP_STATE_DFU;
	}
	if (!observation->attached) {
		return APP_STATE_SEARCHING;
	}
	return APP_STATE_HEALTHY;
}

void app_state_init(struct app_state_machine *machine,
		    const struct app_state_observation *observation)
{
	machine->current = app_state_select(observation);
	machine->entered_ms = observation->now_ms;
	machine->transitions = 0;
}

struct app_state_transition app_state_update(
	struct app_state_machine *machine,
	const struct app_state_observation *observation)
{
	struct app_state_transition transition = {
		.previous = machine->current,
		.current = app_state_select(observation),
		.entered_ms = machine->entered_ms,
		.changed = false,
	};

	if (transition.current != transition.previous) {
		machine->current = transition.current;
		machine->entered_ms = observation->now_ms;
		machine->transitions++;
		transition.entered_ms = machine->entered_ms;
		transition.changed = true;
	}
	return transition;
}

const char *app_state_name(enum app_state state)
{
	switch (state) {
	case APP_STATE_BOOTING:   return "booting";
	case APP_STATE_SEARCHING: return "searching";
	case APP_STATE_HEALTHY:   return "healthy";
	case APP_STATE_DFU:       return "dfu";
	case APP_STATE_RECOVERY:  return "recovery";
	default:                  return "unknown";
	}
}
