#include "usb_sleep.h"

void usb_sleep_init(struct usb_sleep_policy *policy, int64_t now_ms,
		    bool usb_present)
{
	*policy = (struct usb_sleep_policy) {
		.usb_present = usb_present,
		.knocks = 0,
		.attached_ms = usb_present ? now_ms : -1,
		.deadline_ms = -1,
	};
}

enum usb_sleep_event usb_sleep_update(struct usb_sleep_policy *policy,
				      int64_t now_ms, bool usb_present)
{
	if (policy->knocks > 0 &&
	    (now_ms < 0 || now_ms > policy->deadline_ms)) {
		policy->knocks = 0;
		policy->deadline_ms = -1;
		if (usb_present == policy->usb_present) {
			return USB_SLEEP_CANCELLED;
		}
	}

	if (usb_present == policy->usb_present) {
		return USB_SLEEP_NONE;
	}
	policy->usb_present = usb_present;
	if (usb_present) {
		policy->attached_ms = now_ms;
		return USB_SLEEP_NONE;
	}

	int64_t pulse_ms = now_ms - policy->attached_ms;
	bool valid_pulse = policy->attached_ms >= 0 && pulse_ms >= USB_SLEEP_MIN_PULSE_MS &&
			   pulse_ms <= USB_SLEEP_MAX_PULSE_MS;

	policy->attached_ms = -1;
	if (!valid_pulse) {
		bool was_armed = policy->knocks > 0;

		policy->knocks = 0;
		policy->deadline_ms = -1;
		return was_armed ? USB_SLEEP_CANCELLED : USB_SLEEP_NONE;
	}
	if (policy->knocks == 0) {
		policy->knocks = 1;
		policy->deadline_ms = now_ms + USB_SLEEP_SEQUENCE_MS;
		return USB_SLEEP_ARMED;
	}

	policy->knocks = 0;
	policy->deadline_ms = -1;
	return USB_SLEEP_ENTER;
}
