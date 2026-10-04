#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_state.h"

static struct app_state_observation observation(int64_t now_ms)
{
	return (struct app_state_observation) {
		.now_ms = now_ms,
	};
}

static void expect_update(struct app_state_machine *machine,
			  struct app_state_observation *input,
			  enum app_state expected, bool changed)
{
	struct app_state_transition transition = app_state_update(machine, input);

	assert(transition.current == expected);
	assert(transition.changed == changed);
	assert(machine->current == expected);
}

static void test_ordinary_boot_and_mesh_transitions(void)
{
	struct app_state_observation input = observation(0);
	struct app_state_machine machine;

	app_state_init(&machine, &input);
	assert(machine.current == APP_STATE_BOOTING);
	assert(machine.transitions == 0);

	input.now_ms = APP_STATE_BOOT_STATUS_MS - 1;
	expect_update(&machine, &input, APP_STATE_BOOTING, false);
	input.now_ms = APP_STATE_BOOT_STATUS_MS;
	expect_update(&machine, &input, APP_STATE_SEARCHING, true);

	input.now_ms++;
	input.attached = true;
	expect_update(&machine, &input, APP_STATE_HEALTHY, true);
	input.now_ms++;
	input.attached = false;
	expect_update(&machine, &input, APP_STATE_SEARCHING, true);
	assert(machine.transitions == 3);
}

/* The deployed topology has NO border router. An attached node must therefore
 * stay HEALTHY — and keep rendering its normal composite — for as long as it
 * runs, no matter that infrastructure is never seen. This is the regression
 * guard for the bug where nodes meshed fine but blanked their patterns to an
 * "isolated" indicator whenever the border router was absent. */
static void test_healthy_without_any_border_router(void)
{
	struct app_state_observation input = observation(10000);
	struct app_state_machine machine;

	input.attached = true;
	app_state_init(&machine, &input);
	assert(machine.current == APP_STATE_HEALTHY);

	/* well past any former grace window, and then some */
	for (int i = 0; i < 100; i++) {
		input.now_ms += 60000;
		expect_update(&machine, &input, APP_STATE_HEALTHY, false);
	}
	assert(machine.transitions == 0);
}

static void test_recovery_and_dfu_priorities(void)
{
	struct app_state_observation input = observation(100);
	struct app_state_machine machine;

	input.recovery_boot = true;
	input.dfu_active = true;
	app_state_init(&machine, &input);
	assert(machine.current == APP_STATE_RECOVERY);

	input.now_ms = APP_STATE_BOOT_STATUS_MS;
	expect_update(&machine, &input, APP_STATE_DFU, true);
	input.now_ms++;
	input.dfu_active = false;
	expect_update(&machine, &input, APP_STATE_SEARCHING, true);

	input.now_ms++;
	input.attached = true;
	expect_update(&machine, &input, APP_STATE_HEALTHY, true);
	input.now_ms++;
	input.dfu_active = true;
	expect_update(&machine, &input, APP_STATE_DFU, true);
	input.now_ms++;
	input.dfu_active = false;
	expect_update(&machine, &input, APP_STATE_HEALTHY, true);
}

static void test_names(void)
{
	assert(strcmp(app_state_name(APP_STATE_BOOTING), "booting") == 0);
	assert(strcmp(app_state_name(APP_STATE_SEARCHING), "searching") == 0);
	assert(strcmp(app_state_name(APP_STATE_HEALTHY), "healthy") == 0);
	assert(strcmp(app_state_name(APP_STATE_DFU), "dfu") == 0);
	assert(strcmp(app_state_name(APP_STATE_RECOVERY), "recovery") == 0);
	assert(strcmp(app_state_name((enum app_state)99), "unknown") == 0);
}

int main(void)
{
	test_ordinary_boot_and_mesh_transitions();
	test_healthy_without_any_border_router();
	test_recovery_and_dfu_priorities();
	test_names();
	puts("app state tests passed");
	return 0;
}
