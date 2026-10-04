#include <assert.h>
#include <stdio.h>

#include "usb_display.h"

static void test_usb_present_at_boot_defaults_quiet(void)
{
	struct usb_display_policy policy;

	usb_display_init(&policy, 100, true);
	assert(policy.usb_present);
	assert(policy.quiet);
	assert(!policy.saw_disconnect);
	assert(!usb_display_update(&policy, 10000, true));
	assert(policy.quiet);
}

static void test_first_connection_defaults_quiet(void)
{
	struct usb_display_policy policy;

	usb_display_init(&policy, 0, false);
	assert(!policy.quiet);
	assert(usb_display_update(&policy, 1000, true));
	assert(policy.usb_present);
	assert(policy.quiet);
}

static void test_quick_reconnect_selects_active_usb_session(void)
{
	struct usb_display_policy policy;

	usb_display_init(&policy, 0, true);
	assert(usb_display_update(&policy, 5000, false));
	assert(!policy.quiet);
	assert(usb_display_update(&policy, 24999, true));
	assert(!policy.quiet);

	/* Active mode stays latched for this USB session after the window. */
	assert(!usb_display_update(&policy, 60000, true));
	assert(!policy.quiet);
}

static void test_reconnect_boundary_and_timeout(void)
{
	struct usb_display_policy policy;

	usb_display_init(&policy, 0, true);
	assert(usb_display_update(&policy, 1000, false));
	assert(usb_display_update(&policy, 21000, true));
	assert(!policy.quiet);

	assert(usb_display_update(&policy, 22000, false));
	assert(usb_display_update(&policy, 42001, true));
	assert(policy.quiet);
}

static void test_reversed_clock_is_not_a_quick_reconnect(void)
{
	struct usb_display_policy policy;

	usb_display_init(&policy, 0, true);
	assert(usb_display_update(&policy, 5000, false));
	assert(usb_display_update(&policy, 4999, true));
	assert(policy.quiet);
}

int main(void)
{
	test_usb_present_at_boot_defaults_quiet();
	test_first_connection_defaults_quiet();
	test_quick_reconnect_selects_active_usb_session();
	test_reconnect_boundary_and_timeout();
	test_reversed_clock_is_not_a_quick_reconnect();
	puts("USB display tests passed");
	return 0;
}
