#include <assert.h>
#include <stdio.h>

#include "usb_sleep.h"

static void test_two_short_usb_pulses_request_sleep(void)
{
	struct usb_sleep_policy policy;

	usb_sleep_init(&policy, 0, false);
	assert(usb_sleep_update(&policy, 1000, true) == USB_SLEEP_NONE);
	assert(usb_sleep_update(&policy, 2000, false) == USB_SLEEP_ARMED);
	assert(policy.knocks == 1);
	assert(usb_sleep_update(&policy, 4000, true) == USB_SLEEP_NONE);
	assert(usb_sleep_update(&policy, 5000, false) == USB_SLEEP_ENTER);
	assert(policy.knocks == 0);
}

static void test_normal_charging_does_not_arm(void)
{
	struct usb_sleep_policy policy;

	usb_sleep_init(&policy, 0, false);
	assert(usb_sleep_update(&policy, 1000, true) == USB_SLEEP_NONE);
	assert(usb_sleep_update(&policy, 9001, false) == USB_SLEEP_NONE);
	assert(policy.knocks == 0);
}

static void test_human_scale_four_second_pulses_request_sleep(void)
{
	struct usb_sleep_policy policy;

	usb_sleep_init(&policy, 0, false);
	assert(usb_sleep_update(&policy, 1000, true) == USB_SLEEP_NONE);
	assert(usb_sleep_update(&policy, 4570, false) == USB_SLEEP_ARMED);
	assert(usb_sleep_update(&policy, 8600, true) == USB_SLEEP_NONE);
	assert(usb_sleep_update(&policy, 12680, false) == USB_SLEEP_ENTER);
}

static void test_short_glitch_does_not_arm(void)
{
	struct usb_sleep_policy policy;

	usb_sleep_init(&policy, 0, false);
	assert(usb_sleep_update(&policy, 1000, true) == USB_SLEEP_NONE);
	assert(usb_sleep_update(&policy, 1249, false) == USB_SLEEP_NONE);
	assert(policy.knocks == 0);
}

static void test_armed_sequence_times_out(void)
{
	struct usb_sleep_policy policy;

	usb_sleep_init(&policy, 0, false);
	assert(usb_sleep_update(&policy, 1000, true) == USB_SLEEP_NONE);
	assert(usb_sleep_update(&policy, 2000, false) == USB_SLEEP_ARMED);
	assert(usb_sleep_update(&policy, 22001, false) == USB_SLEEP_CANCELLED);
	assert(policy.knocks == 0);
	assert(usb_sleep_update(&policy, 23000, true) == USB_SLEEP_NONE);
	assert(usb_sleep_update(&policy, 24000, false) == USB_SLEEP_ARMED);
}

static void test_bad_second_pulse_cancels(void)
{
	struct usb_sleep_policy policy;

	usb_sleep_init(&policy, 0, true);
	assert(usb_sleep_update(&policy, 1000, false) == USB_SLEEP_ARMED);
	assert(usb_sleep_update(&policy, 2000, true) == USB_SLEEP_NONE);
	assert(usb_sleep_update(&policy, 10001, false) == USB_SLEEP_CANCELLED);
	assert(policy.knocks == 0);
}

int main(void)
{
	test_two_short_usb_pulses_request_sleep();
	test_normal_charging_does_not_arm();
	test_human_scale_four_second_pulses_request_sleep();
	test_short_glitch_does_not_arm();
	test_armed_sequence_times_out();
	test_bad_second_pulse_cancels();
	puts("USB sleep tests passed");
	return 0;
}
